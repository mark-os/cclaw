/* request_config tool — handled inline by parent process.
 * Actions: grant_tool, grant_host, grant_path, rename_agent (gated: create an
 * approval and return NULL to park), set_mode (applies immediately, tightens
 * a granted tool's approval mode — no park). */
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
    "\"action\":{\"type\":\"string\",\"enum\":[\"grant_tool\",\"grant_host\",\"grant_path\",\"rename_agent\",\"set_mode\"],"
    "\"description\":\"Type of config request\"},"
    "\"tool\":{\"type\":\"string\",\"description\":\"Tool name (for grant_tool / set_mode)\"},"
    "\"host\":{\"type\":\"string\",\"description\":\"Hostname to allow (for grant_host)\"},"
    "\"path\":{\"type\":\"string\",\"description\":\"Absolute path to grant (for grant_path)\"},"
    "\"name\":{\"type\":\"string\",\"description\":\"New agent name (for rename_agent)\"},"
    "\"preamble\":{\"type\":\"string\",\"description\":\"New system prompt preamble (for rename_agent, optional)\"},"
    "\"mode\":{\"type\":\"string\",\"enum\":[\"always\",\"tool_decides\"],\"description\":\"Approval mode for set_mode: 'always' parks every call, 'tool_decides' uses the tool's predicate\"}"
    "},\"required\":[\"action\"]}";

/* Build canonical args JSON for the approval row using SQLite json_object
 * for safe escaping of model-controlled text. Caller frees. */
static char *build_args_json(sqlite3 *db, const char *action, const char *key,
                             const char *value, const char *preamble) {
    sqlite3_stmt *s;
    const char *sql = preamble
        ? "SELECT json_object('action',?1,?2,?3,'preamble',?4)"
        : "SELECT json_object('action',?1,?2,?3)";
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(s, 1, action, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, value, -1, SQLITE_STATIC);
    if (preamble)
        sqlite3_bind_text(s, 4, preamble, -1, SQLITE_STATIC);
    char *result = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *txt = (const char *)sqlite3_column_text(s, 0);
        if (txt) result = strdup(txt);
    }
    sqlite3_finalize(s);
    return result;
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
        char *args = build_args_json(ctx->db, "grant_tool", "tool", tool, NULL);
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
        char *args = build_args_json(ctx->db, "grant_host", "host", host, NULL);
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
        char *args = build_args_json(ctx->db, "grant_path", "path", path, NULL);
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
        char *args = build_args_json(ctx->db, "rename_agent", "name", new_name, preamble);
        int64_t aid = approval_create(ctx->db, ctx->session_id,
            ctx->current_tool_call_id, "request_config", "rename_agent", args);
        free(args);
        tool_parse_free(&ta);
        if (aid < 0) return strdup("error: failed to create approval");
        session_set_state(ctx->db, ctx->session_id, "awaiting_approval");
        return NULL; /* park */

    } else if (strcmp(act, "set_mode") == 0) {
        /* Tighten oversight on an already-granted tool. Increasing scrutiny
         * (silent→always/tool_decides) needs no approval; loosening to silent
         * happens through the "approve always" decision, not here. */
        const char *tool = targ_str(&ta, "tool");
        const char *mode = targ_str(&ta, "mode");
        if (!tool || !tool[0]) { tool_parse_free(&ta); return strdup("error: 'tool' required for set_mode"); }
        if (!mode || !mode[0]) { tool_parse_free(&ta); return strdup("error: 'mode' required for set_mode (always or tool_decides)"); }
        if (strcmp(mode, "always") != 0 && strcmp(mode, "tool_decides") != 0) {
            tool_parse_free(&ta);
            return strdup("error: set_mode only tightens (always or tool_decides); relax via approving with 'always'");
        }
        int rc = agent_config_set_tool_mode(ctx->db, ctx->agent_name, tool, mode);
        tool_parse_free(&ta);
        if (rc != 0) return strdup("error: set_mode failed (is the tool granted?)");
        char ok[128];
        snprintf(ok, sizeof(ok), "ok: %s now parks per mode '%s'", tool, mode);
        return strdup(ok);

    } else {
        tool_parse_free(&ta);
        return strdup("error: action must be grant_tool, grant_host, grant_path, rename_agent, or set_mode");
    }
}

int tool_request_config_register(ToolRegistry *reg, RequestConfigCtx *ctx) {
    return tools_register(reg, "request_config",
        "Request a configuration change. Grants require human approval; set_mode "
        "applies immediately. Actions: grant_tool (enable shell_exec, web_fetch, "
        "db_query), grant_host (add hostname to allowed_hosts), grant_path (grant "
        "read/write access to an absolute path), rename_agent (rename this agent, "
        "with optional preamble), set_mode (require approval before each call of a "
        "granted tool: mode 'always' or 'tool_decides').",
        PARAMS_JSON, handler, ctx);
}
