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
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Known provider defaults — eager validation + default fill for
 * set_provider, so a definition that can't apply never parks. */
static const struct {
    const char *name;
    const char *base_url;
    const char *model;
} PROVIDERS[] = {
    {"openrouter", "https://openrouter.ai/api/v1", "deepseek/deepseek-v4-flash"},
    {"gemini",     "https://generativelanguage.googleapis.com/v1beta/openai", "gemini-2.5-flash"},
    {"anthropic",  "https://api.anthropic.com/v1", "claude-sonnet-4-20250514"},
};
#define PROVIDER_COUNT (sizeof(PROVIDERS) / sizeof(PROVIDERS[0]))

static const char *PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"action\":{\"type\":\"string\",\"enum\":[\"grant_tool\",\"grant_host\",\"grant_path\",\"rename_agent\",\"set_config\",\"set_provider\"],"
    "\"description\":\"Type of config request\"},"
    "\"key\":{\"type\":\"string\",\"description\":\"Config key (for set_config; must be a registered key — see search_config)\"},"
    "\"value\":{\"type\":\"string\",\"description\":\"New value (for set_config)\"},"
    "\"tool\":{\"type\":\"string\",\"description\":\"Tool name (for grant_tool)\"},"
    "\"host\":{\"type\":\"string\",\"description\":\"Hostname to allow (for grant_host). Prefix with '.' to cover all subdomains: '.example.com' covers example.com AND sub.example.com\"},"
    "\"path\":{\"type\":\"string\",\"description\":\"Absolute path to grant (for grant_path)\"},"
    "\"name\":{\"type\":\"string\",\"description\":\"New agent name (for rename_agent)\"},"
    "\"preamble\":{\"type\":\"string\",\"description\":\"New system prompt preamble (for rename_agent, optional)\"},"
    "\"mode\":{\"type\":\"string\",\"enum\":[\"read\",\"write\"],\"description\":\"Access mode for grant_path (default read)\"},"
    "\"provider\":{\"type\":\"string\",\"description\":\"Provider name (for set_provider): openrouter, gemini, anthropic, or a custom name\"},"
    "\"base_url\":{\"type\":\"string\",\"description\":\"Provider base URL (for set_provider; required for unknown providers)\"},"
    "\"model\":{\"type\":\"string\",\"description\":\"Default model (for set_provider; optional, provider default if omitted)\"},"
    "\"api_key_env\":{\"type\":\"string\",\"description\":\"Secret NAME holding the API key (for set_provider; defaults to <PROVIDER>_API_KEY). Store the key first with save_secret — never pass key material here\"},"
    "\"reason\":{\"type\":\"string\",\"description\":\"Short justification shown to the human approver (optional, recommended)\"}"
    "},\"required\":[\"action\"]}";

/* Build canonical args JSON for the approval row using SQLite json_object
 * for safe escaping of model-controlled text.  Optionals (any may be NULL)
 * arrive as a struct so per-action call sites name only what they use. */
typedef struct {
    const char *mode;        /* grant_path */
    const char *preamble;    /* rename_agent */
    const char *cfg_value;   /* set_config 'value' */
    const char *base_url;    /* set_provider */
    const char *model;       /* set_provider */
    const char *api_key_env; /* set_provider — secret NAME, never key material */
    const char *reason;      /* all actions */
} ArgsOpt;

static char *build_args_json(sqlite3 *db, const char *action, const char *key,
                             const char *value, const ArgsOpt *opt) {
    /* Compose SQL with sequential placeholders for non-NULL optionals. */
    static const struct { const char *json_key; size_t off; } FIELDS[] = {
        {"mode",        offsetof(ArgsOpt, mode)},
        {"preamble",    offsetof(ArgsOpt, preamble)},
        {"value",       offsetof(ArgsOpt, cfg_value)},
        {"base_url",    offsetof(ArgsOpt, base_url)},
        {"model",       offsetof(ArgsOpt, model)},
        {"api_key_env", offsetof(ArgsOpt, api_key_env)},
        {"reason",      offsetof(ArgsOpt, reason)},
    };
    enum { NFIELDS = sizeof(FIELDS) / sizeof(FIELDS[0]) };
    const char *vals[NFIELDS];
    char sql[384];
    int off = snprintf(sql, sizeof(sql),
                       "SELECT json_object('action',?1,?2,?3");
    int next = 4;
    for (size_t i = 0; i < NFIELDS; i++) {
        vals[i] = *(const char *const *)((const char *)opt + FIELDS[i].off);
        if (vals[i])
            off += snprintf(sql + off, sizeof(sql) - (size_t)off,
                            ",'%s',?%d", FIELDS[i].json_key, next++);
    }
    snprintf(sql + off, sizeof(sql) - (size_t)off, ")");

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(s, 1, action, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, value, -1, SQLITE_STATIC);
    next = 4;
    for (size_t i = 0; i < NFIELDS; i++)
        if (vals[i])
            sqlite3_bind_text(s, next++, vals[i], -1, SQLITE_STATIC);

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
                          const ArgsOpt *opt) {
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

    char *args = build_args_json(ctx->db, action, key, value, opt);
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
    if (!act) { tool_parse_free(&ta); return strdup("error: 'action' required (one of: grant_tool, grant_host, grant_path, rename_agent, set_config, set_provider)"); }

    /* Optional reason — treat empty string as absent. */
    const char *reason = targ_str(&ta, "reason");
    if (reason && !reason[0]) reason = NULL;

    char *result = NULL;
    ArgsOpt opt = { .reason = reason };

    if (strcmp(act, "grant_tool") == 0) {
        const char *tool = targ_str(&ta, "tool");
        if (!tool || !tool[0]) { tool_parse_free(&ta); return strdup("error: 'tool' required for grant_tool"); }
        result = gate_request(ctx, "grant_tool", "tool", tool, &opt);

    } else if (strcmp(act, "grant_host") == 0) {
        const char *host = targ_str(&ta, "host");
        if (!host || !host[0]) { tool_parse_free(&ta); return strdup("error: 'host' required for grant_host"); }
        result = gate_request(ctx, "grant_host", "host", host, &opt);

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
        opt.mode = mode;
        result = gate_request(ctx, "grant_path", "path", path, &opt);

    } else if (strcmp(act, "rename_agent") == 0) {
        const char *new_name = targ_str(&ta, "name");
        const char *preamble = targ_str(&ta, "preamble");
        if (!new_name || !new_name[0]) { tool_parse_free(&ta); return strdup("error: 'name' required"); }
        if (!is_valid_agent_name(new_name)) { tool_parse_free(&ta); return strdup("error: agent name must be PascalCase: start with an uppercase letter, letters and digits only, max 63 chars"); }
        opt.preamble = preamble;
        result = gate_request(ctx, "rename_agent", "name", new_name, &opt);

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
        opt.cfg_value = value;
        result = gate_request(ctx, "set_config", "key", key, &opt);

    } else if (strcmp(act, "set_provider") == 0) {
        const char *prov = targ_str(&ta, "provider");
        const char *base_url = targ_str(&ta, "base_url");
        const char *model = targ_str(&ta, "model");
        const char *key_env = targ_str(&ta, "api_key_env");
        if (!prov || !prov[0]) { tool_parse_free(&ta); return strdup("error: 'provider' required for set_provider"); }
        /* Eager validation + default fill: known providers get default
         * base_url/model; unknown ones must supply base_url. Park clean. */
        int known = -1;
        for (size_t i = 0; i < PROVIDER_COUNT; i++)
            if (strcmp(prov, PROVIDERS[i].name) == 0) { known = (int)i; break; }
        if (known < 0 && (!base_url || !base_url[0])) {
            tool_parse_free(&ta);
            return strdup("error: 'base_url' required for unknown providers");
        }
        if (!base_url || !base_url[0]) base_url = PROVIDERS[known].base_url;
        if (strncmp(base_url, "https://", 8) != 0 &&
            strncmp(base_url, "http://", 7) != 0) {
            tool_parse_free(&ta);
            return strdup("error: base_url must start with http:// or https://");
        }
        if (!model || !model[0]) model = (known >= 0) ? PROVIDERS[known].model : NULL;
        /* api_key_env is a secret NAME, never key material. Default derives
         * <PROVIDER>_API_KEY so config_load's env → kv fallback resolves it. */
        char env_buf[96];
        if (key_env && key_env[0]) {
            int ok = (key_env[0] >= 'A' && key_env[0] <= 'Z');
            for (const char *c = key_env; ok && *c; c++)
                ok = (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_';
            if (!ok) {
                tool_parse_free(&ta);
                return strdup("error: api_key_env must match [A-Z][A-Z0-9_]* — it is the secret's NAME, not the key value");
            }
        } else {
            size_t en = 0;
            for (const char *c = prov; *c && en < sizeof(env_buf) - 12; c++)
                env_buf[en++] = (*c >= 'a' && *c <= 'z') ? (char)(*c - 32)
                              : ((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')) ? *c : '_';
            memcpy(env_buf + en, "_API_KEY", 9);
            key_env = env_buf;
        }
        opt.base_url = base_url;
        opt.model = model;
        opt.api_key_env = key_env;
        result = gate_request(ctx, "set_provider", "provider", prov, &opt);

    } else {
        tool_parse_free(&ta);
        return strdup("error: action must be grant_tool, grant_host, grant_path, rename_agent, set_config, or set_provider");
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
        "discover keys with search_config), set_provider (set up an LLM "
        "provider: openrouter, gemini, anthropic, or a custom name with "
        "base_url; store the API key first via save_secret under "
        "<PROVIDER>_API_KEY or pass its name as api_key_env — never key "
        "material). All actions accept an optional "
        "'reason' shown to the approver.",
        PARAMS_JSON, handler, ctx);
    if (rc == 0)
        tools_set_recipe(reg, "request_config", (ToolRecipe){EXEC_INLINE, SBX_NONE, NULL});
    return rc;
}
