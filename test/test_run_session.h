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

/* Run the agent loop synchronously (no threads, no fork).
 * Calls llm_req() in-process, dispatches tools inline.
 * Returns 0 on success, -1 on error. */
static inline int test_run_session(sqlite3 *db, int64_t session_id, AgentSetup *setup) {
    int iteration = 0;
    while (iteration < TEST_MAX_ITERATIONS) {
        int recall = (iteration == 0) ? 1 : 0;
        int rc = llm_req(db, NULL, session_id, recall);
        if (rc != 0) return -1;

        /* Check for pending tool calls */
        int tc_count = 0;
        PendingToolCall *tcs = db_tool_call_get_pending(db, session_id, &tc_count);
        if (!tcs || tc_count == 0) {
            db_tool_call_free_pending(tcs, tc_count);
            return 0; /* No tools — turn complete */
        }

        /* Dispatch pending tool calls inline */
        for (int i = 0; i < tc_count; i++) {
            PendingToolCall *tc = &tcs[i];
            ToolEntry *te = tools_lookup(&setup->reg, tc->name);
            char *result = NULL;
            int is_err = 0;
            if (te) {
                result = te->handler(tc->arguments, te->user_data, &is_err);
            } else {
                result = strdup("error: tool not found");
                is_err = 1;
            }
            if (!result) { result = strdup("error: null result"); is_err = 1; }

            ToolResult tr = {.tool_call_id = tc->call_id, .content = result};
            Message msg = {.role = ROLE_TOOL, .tool_result = &tr,
                           .tool_name = tc->name, .is_error = is_err};
            int64_t rid = entry_append_with_iteration(db, session_id, &msg, tc->iteration_id);
            db_tool_call_complete_with_result(db, tc->entry_id, tc->call_id, rid);
            free(result);
        }
        db_tool_call_free_pending(tcs, tc_count);
        iteration++;
    }
    return -1; /* max iterations */
}

#endif
