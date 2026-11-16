#define _POSIX_C_SOURCE 200809L
#include "settle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "channel_api.h"
#include "child.h"
#include "config_registry.h"
#include "db.h"
#include "dispatch.h"
#include "log.h"
#include "loop.h"
#include "proc.h"
#include "turn_edit.h"

#define EVENT HOOK_EVENT_TURN_SETTLED

/* Start one hook on the turn as it is now. 0 = a child is running. A hook
 * that cannot be started is one log line and nothing else: no state has
 * changed, and it is not counted as a failure of the script. */
static int settle_spawn(int64_t session_id, const char *agent, const HookRef *h) {
    char *input = turn_edit_input(proc_db(), session_id);
    if (!input) {
        LOG_WARN_("hook " EVENT " ext=%s outcome=not_started reason=no finished turn",
                  h->extension);
        return -1;
    }
    int timeout = config_get_int(proc_db(), "turn_settled_timeout");
    if (timeout <= 0) timeout = config_default_int("turn_settled_timeout");
    char err[256] = "";
    int rc = hook_script_spawn(h, agent, session_id, input, timeout,
                               err, sizeof(err));
    free(input);
    if (rc != 0)
        LOG_WARN_("hook " EVENT " ext=%s outcome=not_started reason=%s",
                  h->extension, err);
    else
        LOG_INFO_("hook " EVENT " fork ext=%s wait=%d timeout=%ds",
                  h->extension, h->wait, timeout);
    return rc;
}

/* Start the agent's next awaited hook after `after` (NULL = the first).
 * Returns 1 when one is now running. A hook that will not start ends the
 * chain: the ones behind it would have seen a turn it was meant to edit. */
static int settle_next(int64_t session_id, const char *agent,
                       const HookRef *after) {
    HookRef h;
    if (hook_next(proc_db(), agent, EVENT, 1, after, &h) != 1) return 0;
    return settle_spawn(session_id, agent, &h) == 0;
}

/* End the hold the way a finished compaction ends: the session goes idle,
 * the compaction check the hold deferred runs against the turn as it now is,
 * and otherwise the idle branch of advance_session takes it from there —
 * gates, inbox drain, next turn. A flip that fails leaves the session
 * 'settling' with no child, which is exactly what the sweep looks for. */
static void settle_release(int64_t session_id, const char *agent) {
    if (session_set_state(proc_db(), session_id, "idle") != 0) {
        LOG_WARN_("settle release failed session=%lld — the sweep retries",
                  (long long)session_id);
        return;
    }
    LOG_INFO_("advance state=settling next=idle session=%lld",
              (long long)session_id);
    if (!turn_end_compact(session_id, agent))
        run_advance(session_id);
}

int turn_settle_begin(int64_t session_id, const char *agent) {
    if (!proc_db() || !agent) return 0;
    char why[96];
    if (!hook_turn_settled_due(proc_db(), session_id, agent, why, sizeof(why))) {
        /* Silent for the common case — an agent with no hook. */
        if (strcmp(why, "no hook") != 0)
            LOG_INFO_("hook " EVENT " skipped reason=%s", why);
        return 0;
    }

    /* Hooks nobody waits for go first, all at once: they see the turn as it
     * was delivered and cannot change it. */
    HookRef h;
    const HookRef *after = NULL;
    while (hook_next(proc_db(), agent, EVENT, 0, after, &h) == 1) {
        settle_spawn(session_id, agent, &h);
        after = &h;
    }

    /* Awaited hooks run one at a time. The child is started before the
     * session is marked: if nothing starts, the session never left idle. */
    if (!settle_next(session_id, agent, NULL)) return 0;
    if (session_set_state(proc_db(), session_id, "settling") != 0) {
        /* Not idle any more (a co-pointed process opened a turn) or the write
         * gave up. The script runs on, unheld, and its reply is discarded. */
        child_hook_unhold(session_id);
        LOG_WARN_("hook " EVENT " could not hold session=%lld — reply will be discarded",
                  (long long)session_id);
        return 0;
    }
    LOG_INFO_("advance state=idle next=settling");
    return 1;
}

void turn_settle_child_done(const SettleChild *sc) {
    sqlite3 *db = proc_db();
    if (!db || !sc) return;
    int64_t sid = sc->session_id;
    cclaw_log_set_ctx(sid, -1, sc->agent);

    /* An awaited hook's reply is only good while this process still holds
     * the session for it. */
    int held = sc->hook.wait &&
        db_scalar_i64(db, "SELECT state='settling' FROM sessions WHERE id=?1;",
                      sid, 0) == 1;

    int failed = sc->failed;
    const char *outcome = "failed";
    char *err = NULL;
    const char *reason = sc->why;
    if (!failed && !sc->hook.wait) {
        outcome = "done";
        reason = "not awaited, output discarded";
    } else if (!failed && !held) {
        outcome = "discarded";
        reason = "session no longer held";
    } else if (!failed) {
        char label[160];
        snprintf(label, sizeof(label), "hook:%s", sc->hook.extension);
        switch (turn_edit_apply(db, sid, sc->reply ? sc->reply : "", label, &err)) {
        case TURN_EDIT_APPLIED:  outcome = "applied";  reason = ""; break;
        case TURN_EDIT_NOTHING:  outcome = "nothing";  reason = ""; break;
        case TURN_EDIT_REJECTED: outcome = "rejected"; failed = 1;
                                   reason = err ? err : "reply refused"; break;
        default:                   outcome = "error";    failed = 1;
                                   reason = err ? err : "apply failed"; break;
        }
    }
    if (failed)
        LOG_WARN_("hook " EVENT " ext=%s outcome=%s reason=%s",
                  sc->hook.extension, outcome, reason);
    else
        LOG_INFO_("hook " EVENT " ext=%s outcome=%s%s%s",
                  sc->hook.extension, outcome, reason[0] ? " reason=" : "", reason);

    /* Three failures in a row, on any sessions: one notice to the operator,
     * the way compaction reports. The agent is never told — it cannot fix an
     * extension's script, and the turn it has is intact. */
    if (hook_record_outcome(db, &sc->hook, EVENT, failed)) {
        char text[512];
        snprintf(text, sizeof(text),
                 "⚠ hook '%s' (" EVENT ") has failed %d times in a row — last: "
                 "%s: %.200s. Turns are left as they were; fix the extension "
                 "or detach it.",
                 sc->hook.extension, HOOK_FAIL_NOTIFY, outcome, reason);
        channel_notify_operator(db, proc_cfg() ? proc_cfg()->db_path : NULL,
                                sid, text);
        LOG_WARN_("hook " EVENT " ext=%s failing — operator notified",
                  sc->hook.extension);
    }
    free(err);
    cclaw_log_clear_ctx();

    if (!held) return;
    if (settle_next(sid, sc->agent, &sc->hook)) return;
    settle_release(sid, sc->agent);
}

void turn_settle_sweep(void) {
    sqlite3 *db = proc_db();
    if (!db) return;
    /* Owner-scoped: a peer's hold is its own business while it lives, and
     * db_recover_stale_sessions' once it does not. Ids are collected first —
     * the release below writes. */
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT id, agent_name FROM sessions"
            " WHERE state='settling' AND owner_instance IS ?1 LIMIT 64;",
            -1, &st, NULL) != SQLITE_OK)
        return;
    const char *inst = proc_instance_id();
    if (inst && inst[0]) sqlite3_bind_text(st, 1, inst, -1, SQLITE_STATIC);
    struct { int64_t id; char agent[64]; } held[64];
    int n = 0;
    while (n < 64 && sqlite3_step(st) == SQLITE_ROW) {
        held[n].id = sqlite3_column_int64(st, 0);
        const char *a = (const char *)sqlite3_column_text(st, 1);
        snprintf(held[n].agent, sizeof(held[n].agent), "%s", a ? a : "");
        n++;
    }
    sqlite3_finalize(st);
    for (int i = 0; i < n; i++) {
        if (child_hook_holds(held[i].id)) continue;
        LOG_WARN_("settle sweep: session=%lld held with no hook child — releasing",
                  (long long)held[i].id);
        settle_release(held[i].id, held[i].agent);
    }
}
