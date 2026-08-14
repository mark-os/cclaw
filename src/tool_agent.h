#ifndef CCLAW_TOOL_AGENT_H
#define CCLAW_TOOL_AGENT_H

/* The launch_agent tool: spawns sub-agents under launch-gate rails — all
 * config-registry keys: agent_max_depth, session_max_active (system-wide
 * existence cap). No queue at this gate — a synchronous caller is present,
 * so calls past a ceiling are refused with the knob named in the error. The
 * execution half (how many sessions *work* at once) is
 * session_max_concurrent, enforced by advance_session's drain gate, which
 * defers instead of refusing. */

#include "tools.h"
#include "db.h"

/* Effective agent_max_depth from the config registry. */
int agent_max_depth(sqlite3 *db);

/* Context passed as user_data to launch_agent/check_session handlers */
typedef struct {
    sqlite3 *db;
    int64_t session_id;
    const char *current_tool_call_id;  /* set before handler call for blocking mode */
} AgentLaunchCtx;

/* Register launch_agent tool. Returns 0 on success. */
int tool_launch_agent_register(ToolRegistry *reg, AgentLaunchCtx *ctx);

/* Handler: parse JSON args, launch agent process */
char *tool_launch_agent_handler(const char *arguments, void *user_data, int *is_error);

/* Register check_session tool. Returns 0 on success. */
int tool_check_session_register(ToolRegistry *reg, AgentLaunchCtx *ctx);

/* Handler: check session state + result */
char *tool_check_session_handler(const char *arguments, void *user_data, int *is_error);



#endif
