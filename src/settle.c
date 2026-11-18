#define _POSIX_C_SOURCE 200809L
#include "settle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "child.h"
#include "config_registry.h"
#include "db.h"
#include "dispatch.h"
#include "log.h"
#include "loop.h"
#include "proc.h"
#include "run_tool.h"
#include "turn_edit.h"
#include "wake.h"

_Static_assert(TURN_EDIT_INPUT_MAX_BYTES == RUNTOOL_ARG_FD_MAX,
               "the largest input a hook may ask for is one the hook vehicle carries");

static int is_stop_event(const char *event) {
    return strcmp(event, HOOK_EVENT_STOP) == 0 ||
           strcmp(event, HOOK_EVENT_SUBAGENT_STOP) == 0;
}

static int hook_timeout(const HookRef *h) {
    if (h->timeout > 0) return h->timeout;
    int t = config_get_int(proc_db(), "hook_timeout");
    return t > 0 ? t : config_default_int("hook_timeout");
}

/* Is the session over its trim threshold? When it is, *ctx gets the numbers
 * a ContextThreshold script decides on, as the JSON object sent as its
 * "context". A session that cannot be measured is not over: C's own check
 * runs after this either way. */
static int over_threshold(int64_t session_id, const char *agent, char *ctx, size_t cap) {
    ContextLimits lim;
    int total = turn_edit_measure(proc_db(), session_id, agent, &lim);
    if (total < 0 || lim.threshold <= 0 || total <= lim.threshold) return 0;
    snprintf(ctx, cap, "{\"tokens\":%d,\"window\":%d,\"trim_threshold\":%d,"
             "\"prune_target\":%d}", total, lim.window, lim.threshold, lim.target);
    return 1;
}

/* Start one hook on the session as it is now. 0 = a child is running. A
 * hook that cannot be started is one log line and nothing else: no state has
 * changed, and it is not counted as a failure of the script. */
static int settle_spawn(int64_t session_id, const char *agent, const HookRef *h,
                        const char *context) {
    int too_big = 0;
    TurnEditInputFor what = { .event = h->event, .need_answer = is_stop_event(h->event),
                              .context = context };
    char *input = turn_edit_input(proc_db(), session_id, &h->input, &what, &too_big);
    if (!input) {
        if (too_big)
            LOG_WARN_("hook %s ext=%s outcome=not_started reason=the newest turn alone"
                      " is larger than the hook's max_bytes (%d)",
                      h->event, h->extension, h->input.max_bytes);
        else
            LOG_WARN_("hook %s ext=%s outcome=not_started reason=no turn to send",
                      h->event, h->extension);
        return -1;
    }
    int timeout = hook_timeout(h);
    char err[256] = "";
    int rc = hook_script_spawn(h, agent, session_id, input, timeout, err, sizeof(err));
    free(input);
    if (rc != 0)
        LOG_WARN_("hook %s ext=%s outcome=not_started reason=%s",
                  h->event, h->extension, err);
    else
        LOG_INFO_("hook %s fork ext=%s async=%d timeout=%ds",
                  h->event, h->extension, !h->wait, timeout);
    return rc;
}

/* The hooks nobody waits for: all started at once, at the start of their
 * step. They see the session as it is and cannot change it. */
static void start_async(int64_t session_id, const char *agent, const char *event,
                        const char *context) {
    HookRef h;
    const HookRef *after = NULL;
    while (hook_next(proc_db(), agent, event, 0, after, &h) == 1) {
        settle_spawn(session_id, agent, &h, context);
        after = &h;
    }
}

/* Start the agent's next awaited hook for `event` after `after` (NULL = the
 * first). Returns 1 when one is now running. A hook that will not start (its
 * script is gone, the turn is over its max_bytes) is passed over: that is a
 * fact about that hook, and the ones behind it still have a session to
 * look at. */
static int start_awaited(int64_t session_id, const char *agent, const char *event,
                         const HookRef *after, const char *context) {
    HookRef h;
    while (hook_next(proc_db(), agent, event, 1, after, &h) == 1) {
        if (settle_spawn(session_id, agent, &h, context) == 0) return 1;
        after = &h;
    }
    return 0;
}

/* Step 1, from `after` (NULL = the step starts). */
static int stop_step(int64_t session_id, const char *agent, const HookRef *after) {
    const char *event = after ? after->event : hook_stop_event(proc_db(), session_id);
    if (!after) {
        char why[96];
        if (!hook_stop_due(proc_db(), session_id, agent, why, sizeof(why))) {
            /* Silent for the common case — an agent with no hook. */
            if (strcmp(why, "no hook") != 0)
                LOG_INFO_("hook %s skipped reason=%s", event, why);
            return 0;
        }
        start_async(session_id, agent, event, NULL);
    }
    return start_awaited(session_id, agent, event, after, NULL);
}

/* Step 2, from `after` (NULL = the step starts). Each hook fires only while
 * the session is over its threshold: one that got it under ends the step. */
static int context_step(int64_t session_id, const char *agent, const HookRef *after) {
    char ctx[192];
    if (!hook_any(proc_db(), agent, HOOK_EVENT_CONTEXT_THRESHOLD) ||
        !over_threshold(session_id, agent, ctx, sizeof(ctx)))
        return 0;
    if (!after) start_async(session_id, agent, HOOK_EVENT_CONTEXT_THRESHOLD, ctx);
    return start_awaited(session_id, agent, HOOK_EVENT_CONTEXT_THRESHOLD, after, ctx);
}

/* Mark the session held for the awaited child just started. The child is
 * started before the session is marked: if nothing starts, the session
 * never moved. */
static int settle_hold(int64_t session_id, const char *from) {
    if (session_set_state(proc_db(), session_id, "settling") != 0) {
        /* Not where we left it any more (a co-pointed process moved it) or
         * the write gave up. The script runs on, unheld, and its reply is
         * discarded. */
        child_hook_unhold(session_id);
        LOG_WARN_("hook could not hold session=%lld — reply will be discarded",
                  (long long)session_id);
        return 0;
    }
    LOG_INFO_("advance state=%s next=settling", from);
    return 1;
}

int turn_settle_begin(int64_t session_id, const char *agent) {
    if (!proc_db() || !agent) return 0;
    if (!stop_step(session_id, agent, NULL) && !context_step(session_id, agent, NULL))
        return 0;
    return settle_hold(session_id, "idle");
}

int iteration_settle_begin(int64_t session_id, const char *agent) {
    if (!proc_db() || !agent) return 0;
    if (!context_step(session_id, agent, NULL)) return 0;
    return settle_hold(session_id, "tool_running");
}

/* End the hold: steps 3 and 4. Between iterations (the iteration count is
 * kept while held; going idle at turn end zeroed it) the session goes back
 * to tool_running and advance_iteration_end runs the C check and sends the
 * next request. At turn end it goes idle, the C check runs against the
 * session as the hooks left it, and the idle branch of advance_session takes
 * it from there — gates, inbox drain, next turn. A flip that fails leaves
 * the session 'settling' with no child, which is exactly what the sweep
 * looks for. */
static void settle_release(int64_t session_id, const char *agent) {
    int mid_turn = db_scalar_i64(proc_db(),
        "SELECT turn_iteration > 0 FROM sessions WHERE id=?1;", session_id, 0) == 1;
    const char *next = mid_turn ? "tool_running" : "idle";
    if (session_set_state(proc_db(), session_id, next) != 0) {
        LOG_WARN_("settle release failed session=%lld — the sweep retries",
                  (long long)session_id);
        return;
    }
    LOG_INFO_("advance state=settling next=%s session=%lld", next, (long long)session_id);
    if (mid_turn) {
        run_iteration_end(session_id);
        return;
    }
    turn_end_trim(session_id, agent);
    run_advance(session_id);
}

/* The hook is disabled: one note to the default agent, who can tell the
 * operator. A disabled ContextThreshold hook leaves the agent on C's trim,
 * which is why anyone is told at all. */
static void notify_disabled(const SettleChild *sc, const char *outcome, const char *reason) {
    sqlite3 *db = proc_db();
    char *agent = db_default_agent_name(db);
    int64_t sid = agent ? db_recent_session(db, agent) : 0;
    const char *slash = strrchr(sc->hook.path, '/');
    const char *handler = slash ? slash + 1 : sc->hook.path;
    char text[768];
    snprintf(text, sizeof(text),
             "The hook '%s' (%s) of extension '%s' failed %d times in a row and has"
             " been disabled — last: %s: %.200s. %s Fix the extension and promote it"
             " again to re-enable the hook. Tell the operator.",
             handler, sc->hook.event,
             sc->hook.extension, HOOK_FAIL_LIMIT, outcome, reason,
             strcmp(sc->hook.event, HOOK_EVENT_CONTEXT_THRESHOLD) == 0
                 ? "Until then sessions over their trim threshold are cut by the"
                   " runtime's fallback, which drops the oldest turns."
                 : "Turns are left as they are.");
    if (sid > 0 && inbox_insert_scanned(db, sid, "system", "hook", text) > 0) {
        wake_session(sid);
        LOG_WARN_("hook %s ext=%s disabled after %d failures — %s told (session %lld)",
                  sc->hook.event, sc->hook.extension, HOOK_FAIL_LIMIT, agent,
                  (long long)sid);
    } else {
        LOG_WARN_("hook %s ext=%s disabled after %d failures — no session to tell",
                  sc->hook.event, sc->hook.extension, HOOK_FAIL_LIMIT);
    }
    free(agent);
}

void turn_settle_child_done(const SettleChild *sc) {
    sqlite3 *db = proc_db();
    if (!db || !sc) return;
    int64_t sid = sc->session_id;
    const char *event = sc->hook.event;
    cclaw_log_set_ctx(sid, -1, sc->agent);

    /* An awaited hook's reply is only good while this process still holds
     * the session for it. */
    int held = sc->hook.wait &&
        db_scalar_i64(db, "SELECT state='settling' FROM sessions WHERE id=?1;",
                      sid, 0) == 1;

    /* What counts: a crash, a timeout, a refused reply. A reply with no edits
     * is a success. A reply nobody can use any more (the hold was lost) and
     * a failure on our side say nothing about the script, and count for
     * neither. */
    int failed = sc->failed, counted = 1;
    const char *outcome = "failed";
    char *err = NULL;
    const char *reason = sc->why;
    if (!failed && !sc->hook.wait) {
        outcome = "done";
        reason = "async, output discarded";
    } else if (!failed && !held) {
        outcome = "discarded";
        reason = "session no longer held";
        counted = 0;
    } else if (!failed) {
        char label[160];
        snprintf(label, sizeof(label), "hook:%s", sc->hook.extension);
        switch (turn_edit_apply(db, sid, sc->reply ? sc->reply : "", label, &err)) {
        case TURN_EDIT_APPLIED:  outcome = "applied";  reason = ""; break;
        case TURN_EDIT_NOTHING:  outcome = "nothing";  reason = ""; break;
        case TURN_EDIT_REJECTED: outcome = "rejected"; failed = 1;
                                   reason = err ? err : "reply refused"; break;
        default:                   outcome = "error";    counted = 0;
                                   reason = err ? err : "apply failed"; break;
        }
    }
    if (failed || !counted)
        LOG_WARN_("hook %s ext=%s outcome=%s reason=%s",
                  event, sc->hook.extension, outcome, reason);
    else
        LOG_INFO_("hook %s ext=%s outcome=%s%s%s",
                  event, sc->hook.extension, outcome, reason[0] ? " reason=" : "", reason);

    /* Three failures in a row, on any sessions, disable the hook: one note to
     * the default agent. The agent whose session it was is never told — it
     * cannot fix an extension's script, and its session is intact. */
    if (counted && hook_record_outcome(db, &sc->hook, failed))
        notify_disabled(sc, outcome, reason);
    free(err);
    cclaw_log_clear_ctx();

    if (!held) return;
    int next = is_stop_event(event)
        ? stop_step(sid, sc->agent, &sc->hook) || context_step(sid, sc->agent, NULL)
        : context_step(sid, sc->agent, &sc->hook);
    if (!next) settle_release(sid, sc->agent);
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
