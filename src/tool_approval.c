#define _POSIX_C_SOURCE 200809L
#include "tool_approval.h"
#include "agent_exit.h"
#include <cJSON.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *APPROVAL_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"type\":{\"type\":\"string\",\"enum\":[\"whitelist_host\",\"create_agent\",\"model_change\",\"tool_enable\"],"
    "\"description\":\"Type of approval request\"},"
    "\"payload\":{\"type\":\"object\",\"description\":\"Request details (e.g. {\\\"host\\\":\\\"api.example.com\\\"})\"}"
    "},\"required\":[\"type\",\"payload\"]}";

static char *tool_approval_handler(const char *arguments, void *user_data) {
    ToolApprovalCtx *ctx = (ToolApprovalCtx *)user_data;
    if (!ctx || !ctx->agent_name)
        return strdup("error: approval_request unavailable (no agent context)");

    cJSON *json = cJSON_Parse(arguments);
    if (!json) return strdup("error: invalid JSON arguments");

    cJSON *type_item = cJSON_GetObjectItemCaseSensitive(json, "type");

    if (!cJSON_IsString(type_item)) {
        cJSON_Delete(json);
        return strdup("error: missing or invalid 'type' field");
    }

    /* Validate type */
    const char *type = type_item->valuestring;
    if (strcmp(type, "whitelist_host") != 0 &&
        strcmp(type, "create_agent") != 0 &&
        strcmp(type, "model_change") != 0 &&
        strcmp(type, "tool_enable") != 0) {
        cJSON_Delete(json);
        return strdup("error: type must be one of: whitelist_host, create_agent, model_change, tool_enable");
    }

    cJSON_Delete(json);

    /* T201/V78: Return sentinel — daemon reads args from tool_call entry */
    return strdup(SENTINEL_APPROVAL "pending");
}

int tool_approval_register(ToolRegistry *reg, ToolApprovalCtx *ctx) {
    return tools_register(reg, "approval_request",
                          "Request admin approval for config/permission changes. "
                          "Types: whitelist_host, create_agent, model_change, tool_enable. "
                          "Agent blocks until admin responds.",
                          APPROVAL_PARAMS_JSON, tool_approval_handler, ctx);
}
