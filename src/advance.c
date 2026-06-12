#define _POSIX_C_SOURCE 200809L
#include "advance.h"
#include "llm_proc.h"
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

AdvanceOutput advance_session(sqlite3 *db, int64_t session_id, int max_iterations) {
    if (max_iterations <= 0) max_iterations = DEFAULT_MAX_ITER;

    /* Read session state + agent */
    char *agent = session_get_agent_name(db, session_id);
    if (!agent)
        return make_output(ADVANCE_ERROR, session_id, NULL, 0);

    /* Read state */
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "SELECT state FROM sessions WHERE id=?",
                           -1, &stmt, NULL) != SQLITE_OK) {
        free(agent);
        return make_output(ADVANCE_ERROR, session_id, NULL, 0);
    }
    sqlite3_bind_int64(stmt, 1, session_id);
    char state[32] = {0};
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *s = (const char *)sqlite3_column_text(stmt, 0);
        if (s) snprintf(state, sizeof(state), "%s", s);
    }
    sqlite3_finalize(stmt);

    if (!state[0]) { free(agent); return make_output(ADVANCE_ERROR, session_id, NULL, 0); }

    int iter = session_get_iteration(db, session_id);

    /* ── State machine ───────────────────────────────────── */

    if (strcmp(state, "waiting") == 0) {
        AdvanceOutput out = make_output(ADVANCE_WAITING, session_id, agent, iter);
        free(agent);
        return out;
    }

    if (strcmp(state, "idle") == 0) {
        /* Idle: check inbox for new work */
        if (inbox_count(db, session_id) > 0) {
            inbox_consume_into_entries(db, session_id, 100);
            session_set_iteration(db, session_id, 0);
            session_set_state(db, session_id, "llm_running");
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
        int tc_state = turn_complete(db, session_id);
        if (tc_state == 1) {
            /* Has pending tool calls */
            int tc_count = 0;
            PendingToolCall *calls = db_tool_call_get_pending(db, session_id, &tc_count);
            if (tc_count > 0) {
                session_set_state(db, session_id, "tool_running");
                AdvanceOutput out = make_output(ADVANCE_DISPATCH_TOOLS, session_id, agent, iter);
                out.tc_count = tc_count;
                out.calls = calls;
                free(agent);
                return out;
            }
            /* No pending calls despite tc_state=1 — fall through to done */
            db_tool_call_free_pending(calls, tc_count);
        }
        if (tc_state == 0) {
            /* Normal stop — turn complete */
            session_set_state(db, session_id, "idle");

            /* Notify parent session if this is a sub-agent */
            SessionParentInfo pi = session_get_parent_info(db, session_id);
            if (pi.parent_session_id > 0) {
                char *result_text = get_response_text(db, session_id);
                if (!result_text) result_text = strdup("(no response)");

                if (pi.parent_tool_call_id) {
                    /* Blocking mode: write tool result for parent's tool call */
                    int64_t parent_turn = db_next_turn_id(db, pi.parent_session_id);
                    ToolResult tr = { .tool_call_id = pi.parent_tool_call_id,
                                      .content = result_text };
                    Message rmsg = { .role = ROLE_TOOL, .tool_result = &tr,
                                     .tool_name = "launch_agent", .is_error = 0 };
                    int64_t rid = entry_append_with_turn(db, pi.parent_session_id,
                                                        &rmsg, parent_turn);
                    (void)rid;
                    db_tool_call_set_status(db, pi.parent_session_id,
                                           pi.parent_tool_call_id, "done", NULL);
                    /* Unpark parent */
                    session_set_state(db, pi.parent_session_id, "tool_running");
                } else {
                    /* Background mode: insert into parent inbox */
                    char payload[256];
                    snprintf(payload, sizeof(payload),
                             "Sub-agent (session %lld) completed: %.*s",
                             (long long)session_id, 200, result_text);
                    inbox_insert(db, pi.parent_session_id, "agent_result", payload);
                }
                wake_session(pi.parent_session_id);
                free(result_text);
                free(pi.parent_tool_call_id);
            }

            AdvanceOutput out = make_output(ADVANCE_DONE, session_id, agent, iter);
            free(agent);
            return out;
        }
        /* Error from turn_complete */
        session_set_state(db, session_id, "idle");
        AdvanceOutput out = make_output(ADVANCE_ERROR, session_id, agent, iter);
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

        /* All tools done — dispatch next LLM iteration */
        int new_iter = session_bump_iteration(db, session_id);
        if (new_iter >= max_iterations) {
            /* Hit iteration cap */
            int64_t turn_id = db_next_turn_id(db, session_id);
            Message msg = { .role = ROLE_ASSISTANT,
                            .content = "error: max iterations reached",
                            .stop_reason = STOP_REASON_ERROR };
            entry_append_with_turn(db, session_id, &msg, turn_id);
            session_set_state(db, session_id, "idle");
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
        /* Compaction finished — go idle, check for queued work */
        session_set_state(db, session_id, "idle");
        if (inbox_count(db, session_id) > 0) {
            inbox_consume_into_entries(db, session_id, 100);
            session_set_iteration(db, session_id, 0);
            session_set_state(db, session_id, "llm_running");
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
