#ifndef CCLAW_TOOL_AGENT_H
#define CCLAW_TOOL_AGENT_H

#include "tools.h"
#include "db.h"

/* Agent launch limits */
#define AGENT_MAX_DEPTH 2
#define AGENT_MAX_PER_PARENT 3
#define AGENT_MAX_TOTAL 10

/* Context passed as user_data to launch_agent/check_agent handlers */
typedef struct {
    sqlite3 *db;
    int64_t session_id;
} AgentLaunchCtx;

/* Register launch_agent tool. Returns 0 on success. */
int tool_launch_agent_register(ToolRegistry *reg, AgentLaunchCtx *ctx);

/* Handler: parse JSON args, launch agent process */
char *tool_launch_agent_handler(const char *arguments, void *user_data);

/* Register check_agent tool. Returns 0 on success. */
int tool_check_agent_register(ToolRegistry *reg, AgentLaunchCtx *ctx);

/* Handler: check agent session state + result */
char *tool_check_agent_handler(const char *arguments, void *user_data);

#endif
