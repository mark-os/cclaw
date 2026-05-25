#define _POSIX_C_SOURCE 200809L
#include "tool_soul.h"
#include "db.h"
#include <cJSON.h>
#include <stdlib.h>
#include <string.h>

static const char *SOUL_EDIT_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"soul\":{\"type\":\"string\",\"description\":\"New soul text (persona/tone) for this agent\"}"
    "},\"required\":[\"soul\"]}";

static char *tool_soul_edit_handler(const char *arguments, void *user_data) {
    ToolSoulCtx *ctx = (ToolSoulCtx *)user_data;
    if (!ctx || !ctx->db || !ctx->agent_name)
        return strdup("error: soul_edit unavailable (no agent context)");

    cJSON *json = cJSON_Parse(arguments);
    if (!json) return strdup("error: invalid JSON arguments");

    cJSON *soul_item = cJSON_GetObjectItemCaseSensitive(json, "soul");
    if (!cJSON_IsString(soul_item)) {
        cJSON_Delete(json);
        return strdup("error: missing or invalid 'soul' field");
    }

    int rc = db_agent_set_soul(ctx->db, ctx->agent_name, soul_item->valuestring);
    cJSON_Delete(json);

    if (rc != 0)
        return strdup("error: failed to update soul (agent may not exist in DB)");

    return strdup("ok: soul updated — takes effect next turn");
}

int tool_soul_register(ToolRegistry *reg, ToolSoulCtx *ctx) {
    return tools_register(reg, "soul_edit",
                          "Replace this agent's soul text (persona/tone). Takes effect next turn.",
                          SOUL_EDIT_PARAMS_JSON, tool_soul_edit_handler, ctx);
}
