#ifndef CCLAW_TOOL_AGENT_H
#define CCLAW_TOOL_AGENT_H

#include "tools.h"
#include "db.h"

/* Agent launch limits */
#define AGENT_MAX_DEPTH 2
#define AGENT_MAX_PER_PARENT 3
#define AGENT_MAX_TOTAL 10

/* Context passed as user_data to launch_agent handler */
typedef struct {
    sqlite3 *db;
    int64_t session_id;
    const char *self_path;  /* path to cclaw binary (CLI fork+exec) */
    int daemon_mode;        /* if true, post to spawn_queue instead of fork */
    const char *tool_call_id; /* current tool_call_id for blocking launch */
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
