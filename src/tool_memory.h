#ifndef CCLAW_TOOL_MEMORY_H
#define CCLAW_TOOL_MEMORY_H

/* The memory-block tools — the agent's CRUD over its own long-term memory
 * blocks. Runs inline (EXEC_THREAD) with a live db handle.
 */

#include "tools.h"
#include "sqlite3.h"

/* Context for memory block tools */
typedef struct {
    sqlite3 *db;
    const char *agent_name;
} ToolMemoryCtx;

/* Register memory_create, memory_add, memory_edit, memory_delete tools. */
int tool_memory_register(ToolRegistry *reg, ToolMemoryCtx *ctx);

#endif
