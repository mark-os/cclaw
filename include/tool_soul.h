#ifndef CCLAW_TOOL_SOUL_H
#define CCLAW_TOOL_SOUL_H

#include "tools.h"
#include "sqlite3.h"

/* Context for soul_edit tool */
typedef struct {
    sqlite3 *db;
    const char *agent_name; /* current agent's name */
} ToolSoulCtx;

/* Register soul_edit tool. */
int tool_soul_register(ToolRegistry *reg, ToolSoulCtx *ctx);

#endif
