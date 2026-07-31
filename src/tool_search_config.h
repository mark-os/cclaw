#ifndef CCLAW_TOOL_SEARCH_CONFIG_H
#define CCLAW_TOOL_SEARCH_CONFIG_H

/* The search_config tool: read-only introspection of an agent's config and
 * the tools available to it.
 */

#include "tools.h"
#include "sqlite3.h"

typedef struct {
    sqlite3 *db;
    const char *agent_name;
    /* Calling session — grants are per agent, but a spawn-frozen tool_filter
     * narrows them per session, so the display needs both to be truthful. */
    int64_t session_id;
} SearchConfigCtx;

int tool_search_config_register(ToolRegistry *reg, SearchConfigCtx *ctx);

#endif
