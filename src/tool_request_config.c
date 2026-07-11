/* request_config tool — handled inline by parent process.
 * Actions: grant_tool, grant_host, grant_path, rename_agent (all gated:
 * create an approval and return NULL to park). */
#define _POSIX_C_SOURCE 200809L
#include "tool_request_config.h"
#include "agent_config.h"
#include "approval.h"
#include "config_registry.h"
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
    "\"action\":{\"type\":\"string\",\"enum\":[\"grant_tool\",\"grant_host\",\"grant_path\",\"rename_agent\",\"set_config\"],"
    "\"description\":\"Type of config request\"},"
    "\"key\":{\"type\":\"string\",\"description\":\"Config key (for set_config; must be a registered key — see search_config)\"},"
    "\"value\":{\"type\":\"string\",\"description\":\"New value (for set_config)\"},"
    "\"tool\":{\"type\":\"string\",\"description\":\"Tool name (for grant_tool)\"},"
    "\"host\":{\"type\":\"string\",\"description\":\"Hostname to allow (for grant_host). Prefix with '.' to cover all subdomains: '.example.com' covers example.com AND sub.example.com\"},"
    "\"path\":{\"type\":\"string\",\"description\":\"Absolute path to grant (for grant_path)\"},"
    "\"name\":{\"type\":\"string\",\"description\":\"New agent name (for rename_agent)\"},"
    "\"preamble\":{\"type\":\"string\",\"description\":\"New system prompt preamble (for rename_agent, optional)\"},"
    "\"mode\":{\"type\":\"string\",\"enum\":[\"read\",\"write\"],\"description\":\"Access mode for grant_path (default read)\"},"
    "\"reason\":{\"type\":\"string\",\"description\":\"Short justification shown to the human approver (optional, recommended)\"}"
    "},\"required\":[\"action\"]}";

/* Build canonical args JSON for the approval row using SQLite json_object
 * for safe escaping of model-controlled text.  Accepts optional mode,
 * preamble, cfg_value ('value' field for set_config), reason (any may be
 * NULL).  Caller frees. */
static char *build_args_json(sqlite3 *db, const char *action, const char *key,
                             const char *value, const char *mode,
                             const char *preamble, const char *cfg_value,
                             const char *reason) {
    /* Compose SQL with sequential placeholders for non-NULL optionals. */
    char sql[256];
    int off = snprintf(sql, sizeof(sql),
                       "SELECT json_object('action',?1,?2,?3");
    int next = 4;
    int mode_pos = 0, preamble_pos = 0, value_pos = 0, reason_pos = 0;
    if (mode) {
        mode_pos = next++;
        off += snprintf(sql + off, sizeof(sql) - (size_t)off,
                        ",'mode',?%d", mode_pos);
    }
    if (preamble) {
        preamble_pos = next++;
        off += snprintf(sql + off, sizeof(sql) - (size_t)off,
                        ",'preamble',?%d", preamble_pos);
    }
    if (cfg_value) {
        value_pos = next++;
        off += snprintf(sql + off, sizeof(sql) - (size_t)off,
                        ",'value',?%d", value_pos);
    }
    if (reason) {
        reason_pos = next++;
        off += snprintf(sql + off, sizeof(sql) - (size_t)off,
                        ",'reason',?%d", reason_pos);
    }
    snprintf(sql + off, sizeof(sql) - (size_t)off, ")");

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(s, 1, action, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, value, -1, SQLITE_STATIC);
    if (mode_pos)
        sqlite3_bind_text(s, mode_pos, mode, -1, SQLITE_STATIC);
    if (preamble_pos)
        sqlite3_bind_text(s, preamble_pos, preamble, -1, SQLITE_STATIC);
    if (value_pos)
        sqlite3_bind_text(s, value_pos, cfg_value, -1, SQLITE_STATIC);
    if (reason_pos)
        sqlite3_bind_text(s, reason_pos, reason, -1, SQLITE_STATIC);

    char *result = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *txt = (const char *)sqlite3_column_text(s, 0);
        if (txt) result = strdup(txt);
    }
    sqlite3_finalize(s);
    return result;
}

/* Unified gate for all request_config actions.  Checks session-scoped denial
 * dedup, builds args JSON, creates approval, parks session.  Returns a heap
 * error string on failure, or NULL to signal "parked". */
static char *gate_request(RequestConfigCtx *ctx, const char *action,
                          const char *key, const char *value,
                          const char *mode, const char *preamble,
                          const char *cfg_value, const char *reason) {
    /* Dedup: same (action, key=value) still pending in this session? A live
     * duplicate would otherwise queue a second "Approval required" prompt
     * for the same grant while the first is unanswered — confusing (two
     * identical prompts, no way to tell which a reply answers) and it
     * orphans the first approval until it expires. A prior *denial* is not
     * blocked here: with admin-routed decisions and a grants/history menu
     * for humans to reconsider, permanently forbidding re-asking in-session
     * is the wrong lever. */
    sqlite3_stmt *chk;
    const char *dedup_sql =
        "SELECT 1 FROM approvals WHERE session_id=?1 AND tool_name='request_config'"
        " AND action=?2 AND state='pending'"
        " AND json_extract(args_json,'$.'||?3)=?4";
    if (sqlite3_prepare_v2(ctx->db, dedup_sql, -1, &chk, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(chk, 1, ctx->session_id);
        sqlite3_bind_text(chk, 2, action, -1, SQLITE_STATIC);
        sqlite3_bind_text(chk, 3, key, -1, SQLITE_STATIC);
        sqlite3_bind_text(chk, 4, value, -1, SQLITE_STATIC);
        if (sqlite3_step(chk) == SQLITE_ROW) {
            sqlite3_finalize(chk);
            return strdup("error: a request for this was already sent and is "
                          "still awaiting the user's yes/no reply — do not "
                          "re-request; wait");
        }
        sqlite3_finalize(chk);
    }

    char *args = build_args_json(ctx->db, action, key, value,
                                 mode, preamble, cfg_value, reason);
    if (!args)
        return strdup("error: failed to build args JSON");

    int64_t aid = approval_create(ctx->db, ctx->session_id,
        ctx->current_tool_call_id, "request_config", action, args, "apply");
    free(args);
    if (aid < 0)
        return strdup("error: failed to create approval");

    session_set_state(ctx->db, ctx->session_id, "awaiting_approval");
    return NULL; /* park */
}

static char *handler(const char *arguments, void *user_data) {
    RequestConfigCtx *ctx = (RequestConfigCtx *)user_data;
    if (!ctx || !ctx->db || !ctx->agent_name)
        return strdup("error: request_config unavailable");

    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0) return strdup("error: invalid JSON");

    const char *act = targ_str(&ta, "action");
    if (!act) { tool_parse_free(&ta); return strdup("error: 'action' required (one of: grant_tool, grant_host, grant_path, rename_agent, set_config)"); }

    /* Optional reason — treat empty string as absent. */
    const char *reason = targ_str(&ta, "reason");
    if (reason && !reason[0]) reason = NULL;

    char *result = NULL;

    if (strcmp(act, "grant_tool") == 0) {
        const char *tool = targ_str(&ta, "tool");
        if (!tool || !tool[0]) { tool_parse_free(&ta); return strdup("error: 'tool' required for grant_tool"); }
        result = gate_request(ctx, "grant_tool", "tool", tool,
                              NULL, NULL, NULL, reason);

    } else if (strcmp(act, "grant_host") == 0) {
        const char *host = targ_str(&ta, "host");
        if (!host || !host[0]) { tool_parse_free(&ta); return strdup("error: 'host' required for grant_host"); }
        result = gate_request(ctx, "grant_host", "host", host,
                              NULL, NULL, NULL, reason);

    } else if (strcmp(act, "grant_path") == 0) {
        const char *path = targ_str(&ta, "path");
        if (!path || !path[0]) { tool_parse_free(&ta); return strdup("error: 'path' required for grant_path"); }
        if (path[0] != '/') { tool_parse_free(&ta); return strdup("error: path must be absolute (start with '/')"); }
        const char *mode = targ_str(&ta, "mode");
        if (!mode || !mode[0]) mode = "read";
        if (strcmp(mode, "read") != 0 && strcmp(mode, "write") != 0) {
            tool_parse_free(&ta);
            return strdup("error: mode must be \"read\" or \"write\"");
        }
        result = gate_request(ctx, "grant_path", "path", path,
                              mode, NULL, NULL, reason);

    } else if (strcmp(act, "rename_agent") == 0) {
        const char *new_name = targ_str(&ta, "name");
        const char *preamble = targ_str(&ta, "preamble");
        if (!new_name || !new_name[0]) { tool_parse_free(&ta); return strdup("error: 'name' required"); }
        if (!is_valid_agent_name(new_name)) { tool_parse_free(&ta); return strdup("error: agent name must be PascalCase: start with an uppercase letter, letters and digits only, max 63 chars"); }
        result = gate_request(ctx, "rename_agent", "name", new_name,
                              NULL, preamble, NULL, reason);

    } else if (strcmp(act, "set_config") == 0) {
        const char *key = targ_str(&ta, "key");
        const char *value = targ_str(&ta, "value");
        if (!key || !key[0]) { tool_parse_free(&ta); return strdup("error: 'key' required for set_config"); }
        if (!value) { tool_parse_free(&ta); return strdup("error: 'value' required for set_config"); }
        /* Eager validation: the key must be registered — in the C registry
         * or extension-registered (config row with a code-owned default).
         * Unknown keys fail now, not at approval time. */
        if (!config_default(key)) {
            sqlite3_stmt *ck;
            int known = 0;
            if (sqlite3_prepare_v2(ctx->db,
                    "SELECT 1 FROM config WHERE key=?1 AND default_value IS NOT NULL",
                    -1, &ck, NULL) == SQLITE_OK) {
                sqlite3_bind_text(ck, 1, key, -1, SQLITE_STATIC);
                known = (sqlite3_step(ck) == SQLITE_ROW);
                sqlite3_finalize(ck);
            }
            if (!known) {
                tool_parse_free(&ta);
                return strdup("error: unknown config key — use search_config to list registered keys");
            }
        }
        result = gate_request(ctx, "set_config", "key", key,
                              NULL, NULL, value, reason);

    } else {
        tool_parse_free(&ta);
        return strdup("error: action must be grant_tool, grant_host, grant_path, rename_agent, or set_config");
    }

    tool_parse_free(&ta);
    return result;
}

int tool_request_config_register(ToolRegistry *reg, RequestConfigCtx *ctx) {
    int rc = tools_register(reg, "request_config",
        "Request a configuration change (requires human approval). "
        "Actions: grant_tool (enable shell_exec, web_fetch, db_query), "
        "grant_host (add hostname to allowed_hosts — prefix with '.' to "
        "cover all subdomains, e.g. '.github.com' covers github.com and "
        "api.github.com), grant_path (grant "
        "read or write access to an absolute path; mode: read|write, "
        "default read), rename_agent (rename this agent, with optional "
        "preamble), set_config (set a registered config key to a value — "
        "discover keys with search_config). All actions accept an optional "
        "'reason' shown to the approver.",
        PARAMS_JSON, handler, ctx);
    if (rc == 0)
        tools_set_recipe(reg, "request_config", (ToolRecipe){EXEC_INLINE, SBX_NONE, NULL});
    return rc;
}
