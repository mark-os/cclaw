#ifndef CCLAW_TOOL_SESSION_CONDENSE_H
#define CCLAW_TOOL_SESSION_CONDENSE_H

/* The session_condense tool — the sanctioned session-edit op: the agent
 * replaces a range of its own completed turns with a summary it wrote, as a
 * hook's `summarise` edit does (turn_edit.c: a role-4 entry spliced into the
 * branch). Deliberately absent from agent_default_tools: it rewrites what
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

/* Delimiter between a summary's prose and the mechanical state coda, so a
 * later reader (human or model) can tell the two apart. */
#define CONDENSE_CODA_MARKER "--- carried state ---"

/* E2: live commitments (open approvals incl. unconsumed tickets, running
 * sub-agent sessions, armed cron one-shots) rendered as the coda appended to
 * the summary. Malloc'd, or NULL when nothing is open. Exposed for tests. */
char *condense_carried_state(sqlite3 *db, int64_t session_id);

/* Exposed for tests. */
char *tool_session_condense_handler(const char *arguments, void *user_data,
                                    int *is_error);

#endif
