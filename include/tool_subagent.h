#ifndef CCLAW_TOOL_SUBAGENT_H
#define CCLAW_TOOL_SUBAGENT_H

#include "tools.h"
#include "db.h"

/* V3 limits */
#define SUBAGENT_MAX_DEPTH 2
#define SUBAGENT_MAX_PER_PARENT 3
#define SUBAGENT_MAX_TOTAL 10

/* Context passed as user_data to spawn_agent handler */
typedef struct {
    sqlite3 *db;
    int64_t session_id;
    int depth;              /* current agent depth (0 = top-level) */
    const char *self_path;  /* path to cclaw binary */
} SubAgentCtx;

/* Register spawn_agent tool. Returns 0 on success. */
int tool_spawn_agent_register(ToolRegistry *reg, SubAgentCtx *ctx);

/* Handler: parse JSON args, fork+exec sub-agent process */
char *tool_spawn_agent_handler(const char *arguments, void *user_data);

/* Register check_agent tool. Returns 0 on success. */
int tool_check_agent_register(ToolRegistry *reg, SubAgentCtx *ctx);

/* Handler: read sub-agent result from DB (V13) */
char *tool_check_agent_handler(const char *arguments, void *user_data);

#endif
