#ifndef CCLAW_TOOL_BOOTSTRAP_H
#define CCLAW_TOOL_BOOTSTRAP_H

#include "tools.h"
#include "sqlite3.h"

/* Context for bootstrap tools (configure_provider, configure_channel, create_agent) */
typedef struct {
    sqlite3 *db;
    int64_t session_id;
    const char *agent_name;
} ToolBootstrapCtx;

/* T190: Register configure_provider tool. */
int tool_configure_provider_register(ToolRegistry *reg, ToolBootstrapCtx *ctx);

/* T191: Register configure_channel tool. */
int tool_configure_channel_register(ToolRegistry *reg, ToolBootstrapCtx *ctx);

/* T192: Register create_agent tool. */
int tool_create_agent_register(ToolRegistry *reg, ToolBootstrapCtx *ctx);

#endif
