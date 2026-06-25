#ifndef CCLAW_TOOL_AGENT_H
#define CCLAW_TOOL_AGENT_H

#include "tools.h"
#include "db.h"

/* Agent launch limits (defaults — overridable via DB config key "agent_max_depth").
 * PER_PARENT bounds how many sub-agents one parent may have in flight at once;
 * with parallel dispatch a single assistant turn can launch several, so this is
 * also the concurrency ceiling per parent (no queue — calls past it are rejected). */
#define AGENT_MAX_DEPTH 2
#define AGENT_MAX_PER_PARENT 8
#define AGENT_MAX_TOTAL 16

/* Read agent_max_depth from DB kv, fallback to AGENT_MAX_DEPTH */
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
char *tool_launch_agent_handler(const char *arguments, void *user_data);

/* Register check_session tool. Returns 0 on success. */
int tool_check_session_register(ToolRegistry *reg, AgentLaunchCtx *ctx);

/* Handler: check session state + result */
char *tool_check_session_handler(const char *arguments, void *user_data);

#endif
