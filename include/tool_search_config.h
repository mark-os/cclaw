#ifndef CCLAW_TOOL_SEARCH_CONFIG_H
#define CCLAW_TOOL_SEARCH_CONFIG_H

#include "tools.h"
#include "sqlite3.h"

typedef struct {
    sqlite3 *db;
    const char *agent_name;
} SearchConfigCtx;

int tool_search_config_register(ToolRegistry *reg, SearchConfigCtx *ctx);

#endif
