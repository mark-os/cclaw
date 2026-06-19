/* request_config tool — handled inline by parent process.
 * Actions: grant_tool, grant_host, grant_path, rename_agent.
 * All gated actions create an approval and return NULL (park signal). */
#define _POSIX_C_SOURCE 200809L
#include "tool_request_config.h"
#include "agent_config.h"
#include "approval.h"
#include "db.h"
#include "validate.h"
#include "tool_parse.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"action\":{\"type\":\"string\",\"enum\":[\"grant_tool\",\"grant_host\",\"grant_path\",\"rename_agent\"],"
    "\"description\":\"Type of config request\"},"
    "\"tool\":{\"type\":\"string\",\"description\":\"Tool name to enable (for grant_tool)\"},"
    "\"host\":{\"type\":\"string\",\"description\":\"Hostname to allow (for grant_host)\"},"
    "\"path\":{\"type\":\"string\",\"description\":\"Absolute path to grant (for grant_path)\"},"
    "\"name\":{\"type\":\"string\",\"description\":\"New agent name (for rename_agent)\"},"
    "\"preamble\":{\"type\":\"string\",\"description\":\"New system prompt preamble (for rename_agent, optional)\"}"
    "},\"required\":[\"action\"]}";

/* Build canonical args JSON for the approval row. Caller frees. */
static char *build_args_json(const char *action, const char *key, const char *value,
                             const char *preamble) {
    /* Simple JSON object — use snprintf since values are validated */
    size_t cap = 256 + (value ? strlen(value) : 0) + (preamble ? strlen(preamble) : 0);
    char *buf = malloc(cap);
    if (!buf) return NULL;
    if (preamble)
        snprintf(buf, cap, "{\"action\":\"%s\",\"%s\":\"%s\",\"preamble\":\"%s\"}",
                 action, key, value, preamble);
    else
        snprintf(buf, cap, "{\"action\":\"%s\",\"%s\":\"%s\"}", action, key, value);
    return buf;
}

static char *handler(const char *arguments, void *user_data) {
    RequestConfigCtx *ctx = (RequestConfigCtx *)user_data;
    if (!ctx || !ctx->db || !ctx->agent_name)
        return strdup("error: request_config unavailable");

    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0) return strdup("error: invalid JSON");

    const char *act = targ_str(&ta, "action");
    if (!act) { tool_parse_free(&ta); return strdup("error: 'action' required (one of: grant_tool, grant_host, grant_path, rename_agent)"); }

    if (strcmp(act, "grant_tool") == 0) {
        const char *tool = targ_str(&ta, "tool");
        if (!tool || !tool[0]) { tool_parse_free(&ta); return strdup("error: 'tool' required for grant_tool"); }
        char *args = build_args_json("grant_tool", "tool", tool, NULL);
        int64_t aid = approval_create(ctx->db, ctx->session_id,
            ctx->current_tool_call_id, "request_config", "grant_tool", args);
        free(args);
        tool_parse_free(&ta);
        if (aid < 0) return strdup("error: failed to create approval");
        session_set_state(ctx->db, ctx->session_id, "awaiting_approval");
        return NULL; /* park */

    } else if (strcmp(act, "grant_host") == 0) {
        const char *host = targ_str(&ta, "host");
        if (!host || !host[0]) { tool_parse_free(&ta); return strdup("error: 'host' required for grant_host"); }
        char *args = build_args_json("grant_host", "host", host, NULL);
        int64_t aid = approval_create(ctx->db, ctx->session_id,
            ctx->current_tool_call_id, "request_config", "grant_host", args);
        free(args);
        tool_parse_free(&ta);
        if (aid < 0) return strdup("error: failed to create approval");
        session_set_state(ctx->db, ctx->session_id, "awaiting_approval");
        return NULL; /* park */

    } else if (strcmp(act, "grant_path") == 0) {
        const char *path = targ_str(&ta, "path");
        if (!path || !path[0]) { tool_parse_free(&ta); return strdup("error: 'path' required for grant_path"); }
        if (path[0] != '/') { tool_parse_free(&ta); return strdup("error: path must be absolute (start with '/')"); }
        char *args = build_args_json("grant_path", "path", path, NULL);
        int64_t aid = approval_create(ctx->db, ctx->session_id,
            ctx->current_tool_call_id, "request_config", "grant_path", args);
        free(args);
        tool_parse_free(&ta);
        if (aid < 0) return strdup("error: failed to create approval");
        session_set_state(ctx->db, ctx->session_id, "awaiting_approval");
        return NULL; /* park */

    } else if (strcmp(act, "rename_agent") == 0) {
        const char *new_name = targ_str(&ta, "name");
        const char *preamble = targ_str(&ta, "preamble");
        if (!new_name || !new_name[0]) { tool_parse_free(&ta); return strdup("error: 'name' required"); }
        if (!is_valid_name(new_name)) { tool_parse_free(&ta); return strdup("error: invalid name (use A-Za-z0-9_- only)"); }
        char *args = build_args_json("rename_agent", "name", new_name, preamble);
        int64_t aid = approval_create(ctx->db, ctx->session_id,
            ctx->current_tool_call_id, "request_config", "rename_agent", args);
        free(args);
        tool_parse_free(&ta);
        if (aid < 0) return strdup("error: failed to create approval");
        session_set_state(ctx->db, ctx->session_id, "awaiting_approval");
        return NULL; /* park */

    } else {
        tool_parse_free(&ta);
        return strdup("error: action must be grant_tool, grant_host, grant_path, or rename_agent");
    }
}

int tool_request_config_register(ToolRegistry *reg, RequestConfigCtx *ctx) {
    return tools_register(reg, "request_config",
        "Request a configuration change (requires approval). "
        "Actions: grant_tool (enable shell_exec, web_fetch, db_query), "
        "grant_host (add hostname to allowed_hosts), "
        "grant_path (grant read/write access to an absolute path), "
        "rename_agent (rename this agent, with optional preamble).",
        PARAMS_JSON, handler, ctx);
}
