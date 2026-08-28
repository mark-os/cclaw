#ifndef CCLAW_TOOL_SESSION_CONDENSE_H
#define CCLAW_TOOL_SESSION_CONDENSE_H

/* The session_condense tool — the sanctioned session-edit op: the agent
 * replaces a range of its own completed turns with a summary it wrote, in
 * exactly the shape overflow compaction produces (a role-4 entry spliced into
 * the branch). Deliberately absent from agent_default_tools: it rewrites what
 * the model will see of its own past, so it is grantable, not baseline.
 */

#include "tools.h"
#include "sqlite3.h"
#include <stdint.h>

/* Context passed as user_data. Runs EXEC_THREAD, so the shim rebuilds this
 * around the thread's own db handle, agent name and session id. */
typedef struct {
    sqlite3 *db;
    int64_t session_id;
    char agent_name[64];
} ToolCondenseCtx;

int tool_session_condense_register(ToolRegistry *reg, ToolCondenseCtx *ctx);

/* Exposed for tests. */
char *tool_session_condense_handler(const char *arguments, void *user_data,
                                    int *is_error);

#endif
