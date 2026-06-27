#define _POSIX_C_SOURCE 200809L
#include "advance.h"
#include "wake.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define DEFAULT_MAX_ITER 25

static AdvanceOutput make_output(AdvanceResult action, int64_t sid,
                                 const char *agent, int iter) {
    AdvanceOutput out = { .action = action, .session_id = sid,
                          .iteration = iter, .tc_count = 0, .calls = NULL };
    if (agent)
        snprintf(out.agent_name, sizeof(out.agent_name), "%s", agent);
    return out;
}

/* Notify a sub-agent's parent that the child finished. Blocking mode writes a
 * ToolResult for the parent's launch_agent call; background mode posts to the
 * parent inbox. Called on every terminal path (normal stop, error, max-iter) so
 * a parent blocked on launch_agent always gets a result and never parks forever.
 * On is_error the parent receives the error text with is_error=1. */
static void notify_parent(sqlite3 *db, int64_t session_id, int is_error) {
    SessionParentInfo pi = session_get_parent_info(db, session_id);
    if (pi.parent_session_id <= 0) {
        free(pi.parent_tool_call_id);
        return;
    }

    char *result_text = get_response_text(db, session_id);
    if (!result_text)
        result_text = strdup(is_error ? "error: sub-agent terminated abnormally"
                                      : "(no response)");

    int txn_ok = (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    if (txn_ok) {
        if (pi.parent_tool_call_id) {
            /* Blocking mode: write tool result for parent's tool call */
            ToolResult tr = { .tool_call_id = pi.parent_tool_call_id,
                              .content = result_text };
            Message rmsg = { .role = ROLE_TOOL, .tool_result = &tr,
                             .tool_name = "launch_agent", .is_error = is_error };
            int64_t rid = entry_append_with_turn(db, pi.parent_session_id, &rmsg, 0);
            db_tool_call_complete_by_call(db, pi.parent_session_id,
                                          pi.parent_tool_call_id, rid);
            /* Unpark parent — but only from a state that permits it. A parent
             * sitting in awaiting_approval or compacting must not be stomped
             * back to tool_running (that double-prompts / re-emits per sibling
             * completion); the eventual wake re-advances it from its own state. */
            char pstate[32] = {0};
            sqlite3_stmt *ps;
            if (sqlite3_prepare_v2(db, "SELECT state FROM sessions WHERE id=?",
                                   -1, &ps, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(ps, 1, pi.parent_session_id);
                if (sqlite3_step(ps) == SQLITE_ROW) {
                    const char *s = (const char *)sqlite3_column_text(ps, 0);
                    if (s) snprintf(pstate, sizeof(pstate), "%s", s);
                }
                sqlite3_finalize(ps);
            }
            if (strcmp(pstate, "awaiting_approval") != 0 &&
                strcmp(pstate, "compacting") != 0)
                session_set_state(db, pi.parent_session_id, "tool_running");
        } else {
            /* Background mode: insert into parent inbox */
            char payload[256];
            snprintf(payload, sizeof(payload),
                     "Sub-agent (session %lld) %s: %.*s",
                     (long long)session_id,
                     is_error ? "failed" : "completed", 200, result_text);
            inbox_insert(db, pi.parent_session_id, "agent_result", payload);
        }

        if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            txn_ok = 0;
        }
    }

    if (txn_ok)
        wake_session(pi.parent_session_id);

    free(result_text);
    free(pi.parent_tool_call_id);
}

AdvanceOutput advance_session(sqlite3 *db, int64_t session_id, int max_iterations) {
    if (max_iterations <= 0) max_iterations = DEFAULT_MAX_ITER;

    /* Read agent_name, state, turn_iteration in one query */
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
            "SELECT agent_name, state, turn_iteration FROM sessions WHERE id=?",
            -1, &stmt, NULL) != SQLITE_OK)
        return make_output(ADVANCE_ERROR, session_id, NULL, 0);
    sqlite3_bind_int64(stmt, 1, session_id);

    char *agent = NULL;
    char state[32] = {0};
    int iter = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *a = (const char *)sqlite3_column_text(stmt, 0);
        const char *s = (const char *)sqlite3_column_text(stmt, 1);
        if (a) agent = strdup(a);
        if (s) snprintf(state, sizeof(state), "%s", s);
        iter = sqlite3_column_int(stmt, 2);
    }
    sqlite3_finalize(stmt);

    if (!agent)
        return make_output(ADVANCE_ERROR, session_id, NULL, 0);
    if (!state[0]) { free(agent); return make_output(ADVANCE_ERROR, session_id, NULL, 0); }

    /* ── State machine ───────────────────────────────────── */

    if (strcmp(state, "awaiting_approval") == 0) {
        AdvanceOutput out = make_output(ADVANCE_WAITING, session_id, agent, iter);
        free(agent);
        return out;
    }

    if (strcmp(state, "idle") == 0) {
        /* Idle: atomically consume inbox + flip to llm_running */
        if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
            free(agent);
            return make_output(ADVANCE_ERROR, session_id, NULL, 0);
        }
        int consumed = inbox_consume_into_entries_locked(db, session_id, 100);
        if (consumed > 0) {
            session_set_iteration(db, session_id, 0);
            session_set_state(db, session_id, "llm_running");
        }
        if (consumed < 0) {
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            free(agent);
            return make_output(ADVANCE_ERROR, session_id, NULL, 0);
        }
        if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            free(agent);
            return make_output(ADVANCE_ERROR, session_id, NULL, 0);
        }
        if (consumed > 0) {
            AdvanceOutput out = make_output(ADVANCE_DISPATCH_LLM, session_id, agent, 0);
            free(agent);
            return out;
        }
        AdvanceOutput out = make_output(ADVANCE_NOOP, session_id, agent, iter);
        free(agent);
        return out;
    }

    if (strcmp(state, "llm_running") == 0) {
        /* LLM finished — check what to do next */
        int tc_count = 0;
        PendingToolCall *calls = db_tool_call_get_pending(db, session_id, &tc_count);
        if (tc_count > 0) {
            /* Has pending tool calls */
            session_set_state(db, session_id, "tool_running");
            AdvanceOutput out = make_output(ADVANCE_DISPATCH_TOOLS, session_id, agent, iter);
            out.tc_count = tc_count;
            out.calls = calls;
            free(agent);
            return out;
        }
        db_tool_call_free_pending(calls, tc_count);

        /* Check if last assistant entry was an error */
        int is_error = 0;
        {   sqlite3_stmt *sr_stmt;
            if (sqlite3_prepare_v2(db,
                    "SELECT stop_reason FROM entries WHERE session_id=?"
                    " AND role=2 ORDER BY id DESC LIMIT 1",
                    -1, &sr_stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(sr_stmt, 1, session_id);
                if (sqlite3_step(sr_stmt) == SQLITE_ROW) {
                    const char *sr = (const char *)sqlite3_column_text(sr_stmt, 0);
                    if (sr && strcmp(sr, "error") == 0) is_error = 1;
                }
                sqlite3_finalize(sr_stmt);
            }
        }

        if (is_error) {
            session_set_state(db, session_id, "idle");
            /* Abnormal stop: still notify a waiting parent so a blocking
             * launch_agent gets an error result instead of hanging. */
            notify_parent(db, session_id, 1);
            AdvanceOutput out = make_output(ADVANCE_ERROR, session_id, agent, iter);
            free(agent);
            return out;
        }

        /* Normal stop — turn complete */
        session_set_state(db, session_id, "idle");

        /* Notify parent session if this is a sub-agent */
        notify_parent(db, session_id, 0);

        AdvanceOutput out = make_output(ADVANCE_DONE, session_id, agent, iter);
        free(agent);
        return out;
    }

    if (strcmp(state, "tool_running") == 0) {
        /* All tools done — check for more, then back to LLM */
        int tc_count = 0;
        PendingToolCall *calls = db_tool_call_get_pending(db, session_id, &tc_count);
        if (tc_count > 0) {
            /* More tools pending (parallel tool calls) */
            AdvanceOutput out = make_output(ADVANCE_DISPATCH_TOOLS, session_id, agent, iter);
            out.tc_count = tc_count;
            out.calls = calls;
            free(agent);
            return out;
        }
        db_tool_call_free_pending(calls, tc_count);

        /* No un-dispatched calls left, but async ones (forked tools or
         * sub-agents) may still be in flight. Stay in tool_running and wait;
         * each completion wakes us again and re-checks. The turn only proceeds
         * to the LLM once every tool_call has a result. */
        if (db_tool_call_any_running(db, session_id)) {
            AdvanceOutput out = make_output(ADVANCE_WAITING, session_id, agent, iter);
            free(agent);
            return out;
        }

        /* All tools done — dispatch next LLM iteration */
        int new_iter = session_bump_iteration(db, session_id);
        if (new_iter >= max_iterations) {
            /* Hit iteration cap */
            Message msg = { .role = ROLE_ASSISTANT,
                            .content = "error: max iterations reached",
                            .stop_reason = STOP_REASON_ERROR };
            entry_append_with_turn(db, session_id, &msg, 0);
            session_set_state(db, session_id, "idle");
            /* Max-iter is a terminal error for a sub-agent too — notify parent. */
            notify_parent(db, session_id, 1);
            AdvanceOutput out = make_output(ADVANCE_DONE, session_id, agent, new_iter);
            free(agent);
            return out;
        }
        session_set_state(db, session_id, "llm_running");
        AdvanceOutput out = make_output(ADVANCE_DISPATCH_LLM, session_id, agent, new_iter);
        free(agent);
        return out;
    }

    if (strcmp(state, "compacting") == 0) {
        /* A wake while compacting is only a *completion* once the compaction
         * job row is gone — the worker deletes it (llm_worker.c) before
         * notifying. Any other wake (inbox insert, sibling sub-agent finish)
         * must not flip us to idle and consume the inbox while the worker is
         * still mutating the branch; stay parked and wait for the real signal. */
        int compaction_in_flight = 0;
        {   sqlite3_stmt *js;
            if (sqlite3_prepare_v2(db,
                    "SELECT 1 FROM llm_jobs WHERE session_id=? AND job_type=1 LIMIT 1",
                    -1, &js, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(js, 1, session_id);
                if (sqlite3_step(js) == SQLITE_ROW) compaction_in_flight = 1;
                sqlite3_finalize(js);
            }
        }
        if (compaction_in_flight) {
            AdvanceOutput out = make_output(ADVANCE_WAITING, session_id, agent, iter);
            free(agent);
            return out;
        }
        /* Compaction finished — atomically go idle + check inbox */
        if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
            free(agent);
            return make_output(ADVANCE_ERROR, session_id, NULL, 0);
        }
        session_set_state(db, session_id, "idle");
        int consumed = inbox_consume_into_entries_locked(db, session_id, 100);
        if (consumed > 0) {
            session_set_iteration(db, session_id, 0);
            session_set_state(db, session_id, "llm_running");
        }
        if (consumed < 0) {
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            free(agent);
            return make_output(ADVANCE_ERROR, session_id, NULL, 0);
        }
        if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            free(agent);
            return make_output(ADVANCE_ERROR, session_id, NULL, 0);
        }
        if (consumed > 0) {
            AdvanceOutput out = make_output(ADVANCE_DISPATCH_LLM, session_id, agent, 0);
            free(agent);
            return out;
        }
        AdvanceOutput out = make_output(ADVANCE_NOOP, session_id, agent, 0);
        free(agent);
        return out;
    }

    if (strcmp(state, "rate_limited") == 0) {
        session_set_state(db, session_id, "idle");
        AdvanceOutput out = make_output(ADVANCE_NOOP, session_id, agent, iter);
        free(agent);
        return out;
    }

    /* Unknown state */
    free(agent);
    return make_output(ADVANCE_ERROR, session_id, NULL, iter);
}
