#ifndef TEST_RUN_SESSION_H
#define TEST_RUN_SESSION_H

#include "db.h"
#include "db_response.h"
#include "config.h"
#include "agent_setup.h"
#include "tools.h"
#include "llm_proc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define TEST_MAX_ITERATIONS 20

/* Run the state machine synchronously (no fork, no epoll).
 * Calls llm_proc_main in-process, dispatches tools inline.
 * Returns final exit code (LLM_EXIT_STOP or LLM_EXIT_ERROR). */
static inline int test_run_session(sqlite3 *db, int64_t session_id, AgentSetup *setup) {
    int iteration = 0;
    while (iteration < TEST_MAX_ITERATIONS) {
        setenv("CCLAW_RECALL", iteration == 0 ? "1" : "0", 1);
        int rc = llm_proc_main(session_id);

        /* Use DB state for dispatch (forward-compatible with worker mode) */
        int tc_state = turn_complete(db, session_id);
        if (tc_state <= 0) {
            /* No tool_calls or error — done */
            return (rc == LLM_EXIT_ERROR || tc_state < 0) ? LLM_EXIT_ERROR : LLM_EXIT_STOP;
        }

        /* Dispatch pending tool calls */
        int tc_count = 0;
        PendingToolCall *tcs = db_tool_call_get_pending(db, session_id, &tc_count);
        if (!tcs || tc_count == 0) return LLM_EXIT_ERROR;

        for (int i = 0; i < tc_count; i++) {
            PendingToolCall *tc = &tcs[i];
            ToolEntry *te = tools_lookup(&setup->reg, tc->name);
            char *result = NULL;
            if (te) {
                result = te->handler(tc->arguments, te->user_data);
            } else {
                result = strdup("error: tool not found");
            }
            if (!result) result = strdup("error: null result");

            ToolResult tr = {.tool_call_id = tc->call_id, .content = result};
            int is_err = (strncmp(result, "error:", 6) == 0);
            Message msg = {.role = ROLE_TOOL, .tool_result = &tr,
                           .tool_name = tc->name, .is_error = is_err};
            int64_t rid = entry_append_with_turn(db, session_id, &msg, tc->turn_id);
            db_tool_call_complete_with_result(db, tc->entry_id, tc->call_id, rid);
            free(result);
        }
        db_tool_call_free_pending(tcs, tc_count);
        iteration++;
    }
    return LLM_EXIT_ERROR; /* max iterations */
}

#endif
