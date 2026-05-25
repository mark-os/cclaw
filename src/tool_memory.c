#define _POSIX_C_SOURCE 200809L
#include "tool_memory.h"
#include "db.h"
#include <cJSON.h>
#include <stdlib.h>
#include <string.h>

static const char *MEMORY_SET_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"memory\":{\"type\":\"string\",\"description\":\"New memory text (durable facts) for this agent\"}"
    "},\"required\":[\"memory\"]}";

static char *tool_memory_set_handler(const char *arguments, void *user_data) {
    ToolMemoryCtx *ctx = (ToolMemoryCtx *)user_data;
    if (!ctx || !ctx->db || !ctx->agent_name)
        return strdup("error: memory_set unavailable (no agent context)");

    cJSON *json = cJSON_Parse(arguments);
    if (!json) return strdup("error: invalid JSON arguments");

    cJSON *mem_item = cJSON_GetObjectItemCaseSensitive(json, "memory");
    if (!cJSON_IsString(mem_item)) {
        cJSON_Delete(json);
        return strdup("error: missing or invalid 'memory' field");
    }

    int rc = db_agent_set_memory(ctx->db, ctx->agent_name, mem_item->valuestring);
    cJSON_Delete(json);

    if (rc != 0)
        return strdup("error: failed to update memory (agent may not exist in DB)");

    return strdup("ok: memory updated — takes effect next turn");
}

int tool_memory_register(ToolRegistry *reg, ToolMemoryCtx *ctx) {
    return tools_register(reg, "memory_set",
                          "Store durable facts for this agent. Replaces current memory. Takes effect next turn.",
                          MEMORY_SET_PARAMS_JSON, tool_memory_set_handler, ctx);
}
