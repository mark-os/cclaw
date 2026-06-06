/* T274/V120: request_config tool — handled inline by turn_loop parent */
#define _POSIX_C_SOURCE 200809L
#include "tool_request_config.h"
#include "tool_parse.h"
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
    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0) return strdup("error: invalid JSON");

    const char *act = targ_str(&ta, "action");
    if (!act) {
        tool_parse_free(&ta);
        return strdup("error: 'action' required");
    }

    if (strcmp(act, "grant_tool") == 0) {
        const char *tool = targ_str(&ta, "tool");
        if (!tool || !tool[0]) {
            tool_parse_free(&ta);
            return strdup("error: 'tool' required for grant_tool");
        }
    } else if (strcmp(act, "grant_host") == 0) {
        const char *host = targ_str(&ta, "host");
        if (!host || !host[0]) {
            tool_parse_free(&ta);
            return strdup("error: 'host' required for grant_host");
        }
    } else {
        tool_parse_free(&ta);
        return strdup("error: action must be grant_tool or grant_host");
    }

    tool_parse_free(&ta);
    /* This handler should never be called directly — turn_loop handles request_config inline */
    return strdup("error: request_config must be handled by parent process");
}

int tool_request_config_register(ToolRegistry *reg) {
    return tools_register(reg, "request_config",
        "Request permission to use a tool or access a host. "
        "Actions: grant_tool (enable a tool like shell_exec, web_fetch, db_query, js_define_tool), "
        "grant_host (add hostname to allowed_hosts for network access).",
        PARAMS_JSON, handler, NULL);
}
