#ifndef CCLAW_TOOL_MEMORY_H
#define CCLAW_TOOL_MEMORY_H

#include "tools.h"
#include "sqlite3.h"

/* Context for memory_set tool (shares shape with ToolSoulCtx) */
typedef struct {
    sqlite3 *db;
    const char *agent_name;
} ToolMemoryCtx;

/* Register memory_set tool. */
int tool_memory_register(ToolRegistry *reg, ToolMemoryCtx *ctx);

#endif
