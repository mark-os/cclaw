#ifndef CCLAW_TOOL_BOOTSTRAP_H
#define CCLAW_TOOL_BOOTSTRAP_H

#include "tools.h"
#include "sqlite3.h"

/* Context for bootstrap tools (create_agent, update_agent) */
typedef struct {
    sqlite3 *db;
    int64_t session_id;
    const char *agent_name;
    const char *current_tool_call_id;  /* set by dispatcher before each call */
} ToolBootstrapCtx;

/* Register create_agent / update_agent tools. */
int tool_create_agent_register(ToolRegistry *reg, ToolBootstrapCtx *ctx);
int tool_update_agent_register(ToolRegistry *reg, ToolBootstrapCtx *ctx);

#endif
