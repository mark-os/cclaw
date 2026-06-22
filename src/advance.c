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

    if (strcmp(state, "awaiting_agent") == 0) {
        AdvanceOutput out = make_output(ADVANCE_WAITING, session_id, agent, iter);
        free(agent);
        return out;
    }

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
            AdvanceOutput out = make_output(ADVANCE_ERROR, session_id, agent, iter);
            free(agent);
            return out;
        }

        /* Normal stop — turn complete */
        session_set_state(db, session_id, "idle");

        /* Notify parent session if this is a sub-agent */
        SessionParentInfo pi = session_get_parent_info(db, session_id);
        if (pi.parent_session_id > 0) {
            char *result_text = get_response_text(db, session_id);
            if (!result_text) result_text = strdup("(no response)");

            int txn_ok = 1;
            if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
                txn_ok = 0;
            }

            if (txn_ok) {
                if (pi.parent_tool_call_id) {
                    /* Blocking mode: write tool result for parent's tool call */
                    ToolResult tr = { .tool_call_id = pi.parent_tool_call_id,
                                      .content = result_text };
                    Message rmsg = { .role = ROLE_TOOL, .tool_result = &tr,
                                     .tool_name = "launch_agent", .is_error = 0 };
                    int64_t rid = entry_append_with_turn(db, pi.parent_session_id,
                                                        &rmsg, 0);
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

        /* All tools done — dispatch next LLM iteration */
        int new_iter = session_bump_iteration(db, session_id);
        if (new_iter >= max_iterations) {
            /* Hit iteration cap */
            Message msg = { .role = ROLE_ASSISTANT,
                            .content = "error: max iterations reached",
                            .stop_reason = STOP_REASON_ERROR };
            entry_append_with_turn(db, session_id, &msg, 0);
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
