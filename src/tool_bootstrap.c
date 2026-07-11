#define _POSIX_C_SOURCE 200809L
#include "tool_bootstrap.h"
#include "db.h"
#include "tool_parse.h"
#include "validate.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Known provider defaults */
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

static const char *CONFIGURE_PROVIDER_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"provider\":{\"type\":\"string\",\"description\":\"Provider name: openrouter, gemini, anthropic, or custom\"},"
    "\"api_key\":{\"type\":\"string\",\"description\":\"API key for the provider\"},"
    "\"base_url\":{\"type\":\"string\",\"description\":\"Base URL (required for custom, optional otherwise)\"},"
    "\"model\":{\"type\":\"string\",\"description\":\"Model name (optional, uses provider default if omitted)\"}"
    "},\"required\":[\"provider\",\"api_key\"]}";

static char *tool_configure_provider_handler(const char *arguments, void *user_data) {
    ToolBootstrapCtx *ctx = (ToolBootstrapCtx *)user_data;
    if (!ctx || !ctx->db)
        return strdup("error: configure_provider unavailable");

    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0)
        return strdup("error: invalid JSON arguments");

    const char *pname = targ_str(&ta, "provider");
    const char *api_key = targ_str(&ta, "api_key");
    const char *base_url = targ_str(&ta, "base_url");
    const char *model = targ_str(&ta, "model");

    if (!pname || !api_key || !api_key[0]) {
        tool_parse_free(&ta);
        return strdup("error: 'provider' and 'api_key' are required");
    }

    /* Validate known provider or custom with base_url; pick up defaults */
    int known = -1;
    for (size_t i = 0; i < PROVIDER_COUNT; i++) {
        if (strcmp(pname, PROVIDERS[i].name) == 0) { known = (int)i; break; }
    }
    if (known < 0 && (!base_url || !base_url[0])) {
        tool_parse_free(&ta);
        return strdup("error: 'base_url' is required for custom providers");
    }
    const char *url = (base_url && base_url[0]) ? base_url : PROVIDERS[known].base_url;
    if (!model || !model[0]) model = (known >= 0) ? PROVIDERS[known].model : NULL;

    /* Key lives in the encrypted kv under the provider's canonical env-var
     * name, so config_load's env → kv fallback resolves it. */
    char env_name[96];
    size_t en = 0;
    for (const char *c = pname; *c && en < sizeof(env_name) - 12; c++)
        env_name[en++] = (*c >= 'a' && *c <= 'z') ? (char)(*c - 32)
                       : ((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')) ? *c : '_';
    memcpy(env_name + en, "_API_KEY", 9);

    if (db_secret_set(ctx->db, env_name, api_key, "operator", "system") != 0) {
        tool_parse_free(&ta);
        return strdup("error: failed to store API key");
    }

    const char *sql =
        "INSERT INTO providers(name, base_url, endpoint_type, api_key_env, default_model, priority)"
        " VALUES(?,?, 'openai', ?, ?, COALESCE((SELECT MAX(priority)+1 FROM providers), 0))"
        " ON CONFLICT(name) DO UPDATE SET base_url=excluded.base_url,"
        "   api_key_env=excluded.api_key_env,"
        "   default_model=COALESCE(excluded.default_model, default_model);";
    sqlite3_stmt *stmt;
    int rc = -1;
    if (sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, pname, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, url, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, env_name, -1, SQLITE_STATIC);
        if (model) sqlite3_bind_text(stmt, 4, model, -1, SQLITE_STATIC);
        rc = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
        sqlite3_finalize(stmt);
    }
    tool_parse_free(&ta);
    if (rc != 0)
        return strdup("error: failed to store provider config");
    return strdup("config applied: configure_provider (key stored in encrypted kv)");
}

static char *tool_configure_channel_handler(const char *arguments, void *user_data);
static char *tool_create_agent_handler(const char *arguments, void *user_data);

/* EXEC_THREAD shims: rebuild a minimal ctx around the thread's live db handle.
 * configure_provider applies directly (providers upsert + encrypted kv);
 * configure_channel/create_agent are validation-only sentinels. */
static char *configure_provider_thread_run(sqlite3 *db, const char *agent_name,
                                           int64_t session_id, const char *args) {
    ToolBootstrapCtx c = {.db = db, .session_id = session_id, .agent_name = agent_name};
    return tool_configure_provider_handler(args, &c);
}
static char *configure_channel_thread_run(sqlite3 *db, const char *agent_name,
                                          int64_t session_id, const char *args) {
    ToolBootstrapCtx c = {.db = db, .session_id = session_id, .agent_name = agent_name};
    return tool_configure_channel_handler(args, &c);
}
static char *create_agent_thread_run(sqlite3 *db, const char *agent_name,
                                     int64_t session_id, const char *args) {
    ToolBootstrapCtx c = {.db = db, .session_id = session_id, .agent_name = agent_name};
    return tool_create_agent_handler(args, &c);
}

int tool_configure_provider_register(ToolRegistry *reg, ToolBootstrapCtx *ctx) {
    int rc = tools_register(reg, "configure_provider",
                          "Set up LLM provider. Stores the API key encrypted in cclaw.db. "
                          "Known providers: openrouter, gemini, anthropic. "
                          "Use 'custom' with base_url for others.",
                          CONFIGURE_PROVIDER_PARAMS,
                          tool_configure_provider_handler, ctx);
    if (rc == 0)
        tools_set_recipe(reg, "configure_provider",
                         (ToolRecipe){EXEC_THREAD, SBX_NONE, configure_provider_thread_run});
    return rc;
}

/* ── configure_channel ────────────────────────────────────────────── */

static const char *CONFIGURE_CHANNEL_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"channel_type\":{\"type\":\"string\",\"description\":\"Channel type: telegram, cli, or custom\"},"
    "\"bot_token\":{\"type\":\"string\",\"description\":\"Bot token (required for telegram)\"},"
    "\"binary_path\":{\"type\":\"string\",\"description\":\"Path to channel binary (required for custom)\"},"
    "\"config\":{\"type\":\"object\",\"description\":\"Key-value config pairs seeded into channel_state\"}"
    "},\"required\":[\"channel_type\"]}";

static char *tool_configure_channel_handler(const char *arguments, void *user_data) {
    ToolBootstrapCtx *ctx = (ToolBootstrapCtx *)user_data;
    if (!ctx)
        return strdup("error: configure_channel unavailable");

    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0)
        return strdup("error: invalid JSON arguments");

    const char *ctype = targ_str(&ta, "channel_type");
    const char *bot_token = targ_str(&ta, "bot_token");
    const char *binary_path = targ_str(&ta, "binary_path");

    if (!ctype || !ctype[0]) {
        tool_parse_free(&ta);
        return strdup("error: 'channel_type' is required");
    }

    if (strcmp(ctype, "telegram") == 0) {
        if (!bot_token || !bot_token[0]) {
            tool_parse_free(&ta);
            return strdup("error: 'bot_token' is required for telegram channel");
        }
    } else if (strcmp(ctype, "cli") == 0) {
        /* cli needs no credentials */
    } else {
        /* Custom channel — needs binary_path */
        if (!binary_path || !binary_path[0]) {
            tool_parse_free(&ta);
            return strdup("error: 'binary_path' required for custom channel type");
        }
    }

    tool_parse_free(&ta);

    /* Don't write cclaw.db — return sentinel, daemon applies on reap */
    return strdup("config applied: configure_channel");
}

int tool_configure_channel_register(ToolRegistry *reg, ToolBootstrapCtx *ctx) {
    int rc = tools_register(reg, "configure_channel",
                          "Set up a communication channel. "
                          "Supported: telegram (requires bot_token), cli, or custom (requires binary_path). "
                          "Optional config object seeds channel_state kv pairs.",
                          CONFIGURE_CHANNEL_PARAMS,
                          tool_configure_channel_handler, ctx);
    if (rc == 0)
        tools_set_recipe(reg, "configure_channel",
                         (ToolRecipe){EXEC_THREAD, SBX_NONE, configure_channel_thread_run});
    return rc;
}

/* ── create_agent ─────────────────────────────────────────────────── */

static const char *CREATE_AGENT_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\",\"description\":\"Agent name (PascalCase: uppercase first letter, letters and digits only)\"},"
    "\"description\":{\"type\":\"string\",\"description\":\"One-line hint: when to delegate to this agent\"},"
    "\"model\":{\"type\":\"string\",\"description\":\"LLM model identifier\"},"
    "\"system_prompt\":{\"type\":\"string\",\"description\":\"Agent persona/system prompt\"},"
    "\"tools\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Tool whitelist (omit for defaults)\"},"
    "\"allowed_hosts\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Allowed network hosts\"},"
    "\"clone_from\":{\"type\":\"string\",\"description\":\"Clone config from existing agent name\"}"
    "},\"required\":[\"name\"]}";

static char *tool_create_agent_handler(const char *arguments, void *user_data) {
    ToolBootstrapCtx *ctx = (ToolBootstrapCtx *)user_data;
    if (!ctx || !ctx->db)
        return strdup("error: create_agent unavailable");

    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0)
        return strdup("error: invalid JSON arguments");

    const char *n = targ_str(&ta, "name");
    if (!n || !n[0]) {
        tool_parse_free(&ta);
        return strdup("error: 'name' is required");
    }

    /* Reject invalid names */
    if (!is_valid_agent_name(n)) {
        tool_parse_free(&ta);
        return strdup("error: agent name must be PascalCase: start with an uppercase letter, letters and digits only, max 63 chars");
    }

    tool_parse_free(&ta);

    /* Return sentinel — daemon reads args from tool_call entry */
    return strdup("config applied: create_agent");
}

int tool_create_agent_register(ToolRegistry *reg, ToolBootstrapCtx *ctx) {
    int rc = tools_register(reg, "create_agent",
                          "Propose creation of a named agent. Requires admin approval. "
                          "On approval, daemon creates agent directory, seeds DB, and binds to channel.",
                          CREATE_AGENT_PARAMS,
                          tool_create_agent_handler, ctx);
    if (rc == 0)
        tools_set_recipe(reg, "create_agent",
                         (ToolRecipe){EXEC_THREAD, SBX_NONE, create_agent_thread_run});
    return rc;
}
