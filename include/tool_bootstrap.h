#ifndef CCLAW_TOOL_BOOTSTRAP_H
#define CCLAW_TOOL_BOOTSTRAP_H

#include "tools.h"
#include "sqlite3.h"

/* Context for bootstrap tools (configure_provider, configure_channel, create_agent) */
typedef struct {
    sqlite3 *db;
} ToolBootstrapCtx;

/* T190: Register configure_provider tool. */
int tool_configure_provider_register(ToolRegistry *reg, ToolBootstrapCtx *ctx);

#endif
