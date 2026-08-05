#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>

#include "loop.h"

#include "advance.h"
#include "agent_setup.h"
#include "channel_api.h"
#include "child.h"
#include "cli.h"
#include "config_registry.h"
#include "context.h"
#include "db.h"
#include "dispatch.h"
#include "llm_worker.h"
#include "log.h"
#include "proc.h"
#include "resolve.h"
#include "types.h"

/* Effective iteration cap for a session: agents.max_iterations (if > 0)
 * overrides global config. run_advance and the dispatch_llm_req fallback must
 * resolve this identically — a global-only fallback caps a raised per-agent
 * limit and kills the turn early. */
int session_max_iter(int64_t session_id) {
    int max_iter = proc_cfg()->max_iterations > 0 ? proc_cfg()->max_iterations
                                              : config_default_int("max_iterations");
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(proc_db(),
            "SELECT a.max_iterations FROM agents a"
            " JOIN sessions s ON s.agent_name = a.name"
            " WHERE s.id = ?", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, session_id);
        if (sqlite3_step(s) == SQLITE_ROW) {
            int ami = sqlite3_column_int(s, 0);
            if (ami > 0) max_iter = ami;
        }
        sqlite3_finalize(s);
    }
    return max_iter;
}

/* ── run_advance: call advance_session and execute the decision ── */

static const char *advance_action_name(AdvanceResult a) {
    switch (a) {
    case ADVANCE_NOOP:           return "noop";
    case ADVANCE_DISPATCH_LLM:   return "dispatch_llm";
    case ADVANCE_DISPATCH_TOOLS: return "dispatch_tools";
    case ADVANCE_DONE:           return "done";
    case ADVANCE_WAITING:        return "waiting";
    case ADVANCE_ERROR:          return "error";
    }
    return "?";
}

void run_advance(int64_t session_id) {
    int max_iter = session_max_iter(session_id);
    /* Tag every log line from here — including advance_session's own state
     * transitions and the dispatch/deliver calls below, which run on this
     * thread — with the advancing session (and, once known, agent), so
     * support logs tie back to the conversation and its llm_responses rows. */
    cclaw_log_set_ctx(session_id, -1, NULL);
    AdvanceOutput out = advance_session(proc_db(), session_id, max_iter);
    LOG_INFO_("advance action=%s iter=%d",
              advance_action_name(out.action), out.iteration);
    cclaw_log_set_ctx(session_id, -1, out.agent_name);

    switch (out.action) {
    case ADVANCE_DISPATCH_LLM:
        if (dispatch_llm_req(session_id, out.agent_name, out.iteration) < 0) {
            /* Only the root CLI session drives the prompt; sub-agents advance
             * silently in the background. */
            if (!proc_is_daemon() && session_id == proc_cli_session()) proc_set_cli_turn_active(0);
        }
        break;
    case ADVANCE_DISPATCH_TOOLS: {
        /* dispatch_tool returns: 1 = inline (result written),
         * 3 = async parallel-safe dispatched (keep going), 0 = async serial
         * dispatched (stop and wait), 2 = parked for approval, <0 = failure.
         * Parallel-safe calls are launched back-to-back; a serial async call
         * (or a park/failure) stops dispatch and we wait for its completion. */
        /* The shared setup serves whichever session is advancing — root or any
         * sub-agent (in the daemon, many agents through one setup). This is the
         * ONLY caps/containment binding point: every batch reloads the
         * advancing agent's grants, sandbox_profile, and shell_path from the
         * DB, so the in-memory snapshot can never go stale (expiry, revoke,
         * rename, update_agent, agent switch). */
        if (proc_tool_setup())
            agent_setup_refresh_caps(proc_tool_setup(), proc_db(), out.agent_name);
        int async_in_flight = 0;
        int stop = 0;
        for (int i = 0; i < out.tc_count && !stop; i++) {
            /* CAS claim (pending → running) before any tool logic runs, so a
             * co-pointed process (daemon + CLI on the same session) can't
             * double-dispatch the same call — the loser skips, the winner
             * owns the call's lifecycle. Paths that don't dispatch after all
             * (approval park, child ceiling) unclaim below so the approval /
             * freed-slot re-advance can re-dispatch. */
            int claim = db_tool_call_claim(proc_db(), session_id, out.calls[i].call_id);
            if (claim == 0) {
                LOG_INFO_("tool claim lost tool=%s (co-pointed dispatcher)",
                          out.calls[i].name);
                continue;
            }
            if (claim < 0) { stop = 1; break; }  /* db error — retry on re-advance */
            int rc = dispatch_tool(session_id, out.agent_name, &out.calls[i]);
            switch (rc) {
            case 1: break;                              /* inline done — next */
            case 3: async_in_flight = 1; break;         /* parallel async — next */
            case 0: async_in_flight = 1; stop = 1; break; /* serial async — wait */
            default:                                    /* parked (2) or failure */
                /* The call didn't dispatch: release the claim (a path that
                 * already wrote a result + 'done' is left alone — CAS). */
                db_tool_call_unclaim(proc_db(), session_id, out.calls[i].call_id);
                /* -1 = child ceiling hit: the call is back to 'pending' and un-
                 * forked. Remember the session so a freed slot (reap) re-advances
                 * it instead of leaving it stuck in tool_running forever. */
                if (rc == -1) stalled_add(session_id);
                stop = 1;
                break;
            }
        }
        /* Apply any auto-decision handle_approval_park recorded — only now is
         * the parked call unclaimed, so the decision lands inside its block
         * window and actually answers the call. */
        approval_flush_deferred();
        /* Only advance now if every call ran inline. If anything async is in
         * flight, its completion (reap or sub-agent finish) re-advances us. */
        if (!async_in_flight && !stop)
            run_advance(session_id);
        db_tool_call_free_pending(out.calls, out.tc_count);
        break;
    }
    case ADVANCE_DONE:
        deliver_response(session_id);
        /* Attempt compaction if configured */
        if (proc_cfg()->compaction && llm_worker_alive() &&
            session_needs_compaction(proc_db(), session_id, proc_cfg())) {
            session_set_state(proc_db(), session_id, "compacting");
            if (llm_worker_submit_compact(proc_db(), session_id, out.agent_name) != 0)
                session_set_state(proc_db(), session_id, "idle");
        }
        if (!proc_is_daemon() && session_id == proc_cli_session()) {
            proc_set_cli_turn_active(0);
        }
        break;
    case ADVANCE_WAITING:
    case ADVANCE_NOOP:
        /* Deferred by the concurrency gate: note the session so a freed
         * resource (reap, worker/tool-thread completion) re-advances it
         * without waiting out a sweep tick — the CLI has no sweep at all. */
        if (out.deferred)
            stalled_add(session_id);
        /* Root turn stays active while its own async work (forked tools or
         * sub-agents) is in flight; awaiting_approval has neither, so the prompt
         * is released for the user's y/n. Sub-agents never touch the root flag. */
        if (!proc_is_daemon() && session_id == proc_cli_session()
            && !child_has_session(session_id)
            && !db_tool_call_any_running(proc_db(), session_id))
            proc_set_cli_turn_active(0);
        break;
    case ADVANCE_ERROR:
        /* In daemon mode this used to vanish silently — log it so a stuck
         * session leaves a trail the user can send. */
        LOG_ERROR_("advance_session failed");
        if (!proc_is_daemon()) {
            fprintf(stderr, "error: session advance failed\n");
            if (session_id == proc_cli_session()) proc_set_cli_turn_active(0);
        }
        break;
    }
    cclaw_log_clear_ctx();
}

void deliver_response(int64_t session_id) {
    if (!proc_is_daemon()) {
        /* CLI stdout belongs to the root session's turn. A sub-agent finishing
         * routes its result to the parent's tool_call (advance.c), not stdout. */
        if (session_id != proc_cli_session()) return;
        cli_indicator_clear();
        char *text = get_response_text(proc_db(), session_id);
        if (text) { printf("%s\n", text); free(text); }
        proc_set_cli_turn_active(0);
        return;
    }

    /* Daemon mode: the outbox rows were written by the channel edge's
     * boundary evaluation (advance_deliver_boundary — policy, quiescence
     * coalescing, 'explicit', ingest-only all live there). This is only the
     * FIFO nudge advance.c can't send (no db_path there); waking with an
     * empty outbox is a harmless no-op for the runner. */
    char *channel = db_scalar_text(proc_db(),
        "SELECT channel_name FROM sessions WHERE id=?;", session_id);
    if (!channel) return;
    if (proc_cfg() && proc_cfg()->db_path) channel_outbox_wake(proc_cfg()->db_path, channel);
    free(channel);
}
