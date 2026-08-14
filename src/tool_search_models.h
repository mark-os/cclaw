#ifndef CCLAW_TOOL_SEARCH_MODELS_H
#define CCLAW_TOOL_SEARCH_MODELS_H

#include "tools.h"
#include "sqlite3.h"

/* search_models — the provider-catalog probe (config-ax Phase 3) as its own
 * tool, deliberately NOT in agent_default_tools: it is the only read-only
 * introspection surface that can put a packet on the wire, so an agent asks
 * for it via request_config like any other ungranted tool. Registered here so
 * it shows up under "Requestable Tools"; the grant is what makes it callable. */
typedef struct {
    sqlite3 *db;
} SearchModelsCtx;

int tool_search_models_register(ToolRegistry *reg, SearchModelsCtx *ctx);

#endif
