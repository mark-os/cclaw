#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "sweep.h"

#include "advance.h"
#include "approval.h"
#include "child.h"
#include "config_registry.h"
#include "cron.h"
#include "db.h"
#include "log.h"
#include "loop.h"
#include "proc.h"
#include "resolve.h"
#include "types.h"
#include "wake.h"

/* ── approval_sweep_expired: deny timed-out pending approvals ── */

void approval_sweep_expired(void) {
    int n = 0;
    int64_t *ids = approval_list_expired(proc_db(), proc_instance_id(), &n);
    for (int i = 0; i < n; i++)
        resolve_approval(ids[i], APPROVAL_DENY, "auto:expired", 0);
    free(ids);
}

/* Unpark one approval past the short block window: answer the frozen tool_call
 * with a non-terminal "still pending" result and resume the turn, leaving the
 * approval pending so a later decision is delivered async (post-window). Under
 * BEGIN IMMEDIATE because db_tool_call_set_status is not itself a CAS — re-check
 * the parked invariant before mutating so a concurrent resolve can't double-act. */
static void approval_unpark_block_window(int64_t approval_id) {
    if (sqlite3_exec(proc_db(), "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK)
        return;

    int64_t session_id = -1;
    char call_id[128] = {0}, tool_name[128] = {0};
    int ok = 0;
    sqlite3_stmt *s;
    const char *sel =
        "SELECT a.session_id, a.tool_call_id, a.tool_name FROM approvals a"
        " JOIN sessions s ON s.id = a.session_id"
        " JOIN tool_calls t ON t.session_id = a.session_id AND t.call_id = a.tool_call_id"
        " WHERE a.id=? AND a.state='pending' AND s.state='awaiting_approval'"
        "   AND t.status='pending';";
    if (sqlite3_prepare_v2(proc_db(), sel, -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, approval_id);
        if (sqlite3_step(s) == SQLITE_ROW) {
            session_id = sqlite3_column_int64(s, 0);
            const char *cid = (const char *)sqlite3_column_text(s, 1);
            const char *tn = (const char *)sqlite3_column_text(s, 2);
            if (cid) snprintf(call_id, sizeof(call_id), "%s", cid);
            if (tn) snprintf(tool_name, sizeof(tool_name), "%s", tn);
            ok = cid != NULL;
        }
        sqlite3_finalize(s);
    }
    if (!ok) { sqlite3_exec(proc_db(), "ROLLBACK;", NULL, NULL, NULL); return; }

    char buf[512];
    approval_background_notice(approval_id, buf, sizeof(buf));
    ToolResult tr = { .tool_call_id = call_id, .content = buf };
    Message msg = { .role = ROLE_TOOL, .tool_result = &tr,
                    .tool_name = tool_name, .is_error = 0 };
    if (entry_append_with_iteration(proc_db(), session_id, &msg, 0) < 0 ||
        db_tool_call_set_status(proc_db(), session_id, call_id, "done", "block_window") != 0 ||
        session_set_state(proc_db(), session_id, "tool_running") != 0) {
        sqlite3_exec(proc_db(), "ROLLBACK;", NULL, NULL, NULL);
        return;
    }
    if (sqlite3_exec(proc_db(), "COMMIT;", NULL, NULL, NULL) != SQLITE_OK)
        return;

    wake_session(session_id);
    run_advance(session_id);
}

/* ── approval_sweep_block_window: unpark approvals past the short block ── */

static void approval_sweep_block_window(void) {
    int block = approval_block_seconds(proc_db());
    int n = 0;
    int64_t *ids = approval_list_block_due(proc_db(), block, proc_instance_id(), &n);
    for (int i = 0; i < n; i++)
        approval_unpark_block_window(ids[i]);
    free(ids);
}

/* ── session_sweep_inbox: backstop for idle sessions with queued work ──
 * Edge wakes (wake pipe / worker / reap) are process-local, so a peer can leave
 * an inbox row on a now-idle session without any live process holding an edge
 * for it (e.g. a cross-process post-window approval delivered by a -p run that
 * then exits). This catches that orphan ≤ POLL_DB_INTERVAL late — a backstop,
 * not the scheduler. advance_session claims an idle session atomically (BEGIN
 * IMMEDIATE + inbox-consume + owner-stamping CAS), so a concurrent edge can't
 * double-dispatch: the loser consumes nothing and NOOPs. Daemon only — a
 * transient CLI is scoped to its own session and must not adopt orphans. */
static void session_sweep_inbox(void) {
    /* Besides queued inbox rows, also pick up sessions whose leaf entry is
     * unanswered: role 1 (a refused dispatch consumed the inbox then parked the
     * session idle/rate_limited, so no inbox row remains to trigger a retry) or
     * role 3 (a mid-turn dispatch bounce left a tool result nobody answered).
     * Advancing them re-runs the dispatch gates once pressure clears
     * (rate_limited flips to idle on the first advance, dispatches on the next
     * tick). Keep this predicate identical to advance_session's idle-branch
     * resume check — a wider one here just wakes sessions that NOOP.
     * A session silenced by the autonomous-turn guard keeps matching (its
     * inbox rows stay queued) and NOOPs at the gate every tick — two indexed
     * counts, no LLM. Cheaper than teaching this SQL the guard's predicate.
     *
     * Stuck-completion arm: a session this instance owns, parked in
     * llm_running/compacting with no llm_jobs row, has nothing in flight —
     * its completion advance lost a write to a BUSY give-up (state flip or
     * pool-full revert; a failed worker job_delete leaves a row and is NOT
     * this shape). advance_session is idempotent there (redundant-wake
     * guard), so re-advancing finishes the completion or re-parks idle.
     * Owner scoping keeps this daemon out of a live CLI session's submit
     * window (state committed, job row not yet inserted — a cross-process
     * gap); dead-owner strands stay db_recover_stale_sessions' job. */
    const char *sql =
        "SELECT s.id FROM sessions s WHERE"
        "  (s.state IN ('idle','rate_limited')"
        "   AND (EXISTS (SELECT 1 FROM inbox i"
        "                WHERE i.session_id=s.id AND i.consumed=0)"
        "     OR EXISTS (SELECT 1 FROM entries e"
        "                WHERE e.id=s.leaf_id AND e.session_id=s.id"
        "                  AND e.role IN (1,3))))"
        "  OR (s.state IN ('llm_running','compacting')"
        "      AND s.owner_instance = ?1"
        "      AND NOT EXISTS (SELECT 1 FROM llm_jobs j"
        "                      WHERE j.session_id = s.id))"
        " LIMIT 64;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(proc_db(), sql, -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, proc_instance_id(), -1, SQLITE_STATIC);
    int cap = 0, n = 0;
    int64_t *ids = NULL;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n >= cap) {
            cap = cap ? cap * 2 : 16;
            int64_t *tmp = realloc(ids, (size_t)cap * sizeof(*ids));
            if (!tmp) { free(ids); sqlite3_finalize(st); return; }
            ids = tmp;
        }
        ids[n++] = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    for (int i = 0; i < n; i++)
        if (!child_has_session(ids[i])) run_advance(ids[i]);
    free(ids);
}

/* ── db_periodic: recurring DB housekeeping. Owner-scoped recovery is safe to
 * repeat here (it reclaims only dead-owned sessions), so CLI and daemon peers
 * can both keep the shared DB consistent. ── */

void db_periodic(void) {
    process_heartbeat(proc_db(), proc_instance_id());
    process_gc_dead(proc_db(), PROCESS_TTL_SEC);
    db_recover_stale_sessions(proc_db());   /* owner-scoped, safe to repeat */
    approval_sweep_block_window();
    approval_sweep_expired();
    db_prune_inbox(proc_db());
    db_prune_outbox(proc_db());
    db_wal_checkpoint(proc_db());   /* truncate WAL — passive checkpoint can stall on a long reader */

    /* Disk floor monitoring: log loudly on crossing the threshold (edge-
     * triggered so it doesn't spam every poll). The dispatch_llm gate does the
     * actual refusing; this makes the low-disk state visible in the journal. */
    static int disk_low = 0;
    int disk_floor_mb = config_default_int("disk_min_free_mb");
    long free_mb = db_free_mb(proc_db());
    if (free_mb >= 0 && disk_floor_mb > 0) {
        if (free_mb < disk_floor_mb && !disk_low) {
            LOG_WARN_("disk_low free_mb=%ld floor_mb=%d refusing new llm dispatch",
                      free_mb, disk_floor_mb);
            disk_low = 1;
        } else if (free_mb >= disk_floor_mb && disk_low) {
            LOG_INFO_("disk_recovered free_mb=%ld floor_mb=%d", free_mb, disk_floor_mb);
            disk_low = 0;
        }
    }

    if (proc_is_daemon()) {
        session_sweep_inbox();
        advance_sweep_undelivered(proc_db());  /* convergence sweep: cursor-lag edges */
        cron_run_due(proc_db());
    }
}
