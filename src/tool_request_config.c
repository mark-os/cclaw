/* T274/V120: request_config tool — CLI inline approval for tool/host grants */
#define _POSIX_C_SOURCE 200809L
#include "tool_request_config.h"
#include "agent_exit.h"
#include <cJSON.h>
#include <stdlib.h>
#include <string.h>

static const char *PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"action\":{\"type\":\"string\",\"enum\":[\"grant_tool\",\"grant_host\"],"
    "\"description\":\"Type of config request\"},"
    "\"tool\":{\"type\":\"string\",\"description\":\"Tool name to enable (for grant_tool)\"},"
    "\"host\":{\"type\":\"string\",\"description\":\"Hostname to allow (for grant_host)\"}"
    "},\"required\":[\"action\"]}";

static char *handler(const char *arguments, void *user_data) {
    (void)user_data;
    cJSON *json = cJSON_Parse(arguments);
    if (!json) return strdup("error: invalid JSON");

    cJSON *action = cJSON_GetObjectItemCaseSensitive(json, "action");
    if (!cJSON_IsString(action)) {
        cJSON_Delete(json);
        return strdup("error: 'action' required");
    }

    const char *act = action->valuestring;
    if (strcmp(act, "grant_tool") == 0) {
        cJSON *tool = cJSON_GetObjectItemCaseSensitive(json, "tool");
        if (!cJSON_IsString(tool) || !tool->valuestring[0]) {
            cJSON_Delete(json);
            return strdup("error: 'tool' required for grant_tool");
        }
    } else if (strcmp(act, "grant_host") == 0) {
        cJSON *host = cJSON_GetObjectItemCaseSensitive(json, "host");
        if (!cJSON_IsString(host) || !host->valuestring[0]) {
            cJSON_Delete(json);
            return strdup("error: 'host' required for grant_host");
        }
    } else {
        cJSON_Delete(json);
        return strdup("error: action must be grant_tool or grant_host");
    }

    cJSON_Delete(json);
    /* Exit code 4 — CLI parent reads args, prompts user, applies */
    return strdup(SENTINEL_CONFIG "request_config");
}

int tool_request_config_register(ToolRegistry *reg) {
    return tools_register(reg, "request_config",
        "Request permission to use a tool or access a host. "
        "Actions: grant_tool (enable a tool like shell_exec, web_fetch, db_query, js_define_tool), "
        "grant_host (add hostname to allowed_hosts for network access).",
        PARAMS_JSON, handler, NULL);
}
