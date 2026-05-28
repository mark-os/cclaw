#define _POSIX_C_SOURCE 200809L
#include "tool_bootstrap.h"
#include "agent_exit.h"
#include "db.h"
#include <cJSON.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* T190: Known provider defaults */
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

    cJSON *json = cJSON_Parse(arguments);
    if (!json) return strdup("error: invalid JSON arguments");

    cJSON *provider = cJSON_GetObjectItemCaseSensitive(json, "provider");
    cJSON *api_key = cJSON_GetObjectItemCaseSensitive(json, "api_key");
    cJSON *base_url = cJSON_GetObjectItemCaseSensitive(json, "base_url");
    cJSON *model = cJSON_GetObjectItemCaseSensitive(json, "model");

    if (!cJSON_IsString(provider) || !cJSON_IsString(api_key) ||
        !api_key->valuestring[0]) {
        cJSON_Delete(json);
        return strdup("error: 'provider' and 'api_key' are required");
    }

    const char *pname = provider->valuestring;
    const char *key = api_key->valuestring;

    /* Determine base_url and model */
    const char *url = NULL;
    const char *mdl = NULL;

    for (size_t i = 0; i < PROVIDER_COUNT; i++) {
        if (strcmp(pname, PROVIDERS[i].name) == 0) {
            url = PROVIDERS[i].base_url;
            mdl = PROVIDERS[i].model;
            break;
        }
    }

    /* Custom provider requires base_url */
    if (!url && (!cJSON_IsString(base_url) || !base_url->valuestring[0])) {
        cJSON_Delete(json);
        return strdup("error: 'base_url' is required for custom providers");
    }

    /* Overrides from arguments */
    if (cJSON_IsString(base_url) && base_url->valuestring[0])
        url = base_url->valuestring;
    if (cJSON_IsString(model) && model->valuestring[0])
        mdl = model->valuestring;

    /* V67: Store API key encrypted in DB */
    if (db_kv_set_secret(ctx->db, "provider.api_key", key) != 0) {
        cJSON_Delete(json);
        return strdup("error: failed to store API key");
    }

    /* Set base_url and model */
    if (url) db_kv_set(ctx->db, "provider.base_url", url);
    if (mdl) db_kv_set(ctx->db, "provider.model", mdl);

    cJSON_Delete(json);

    char buf[256];
    snprintf(buf, sizeof(buf),
             SENTINEL_CONFIG "provider configured: %s (base_url: %s, model: %s)",
             pname, url ? url : "(unchanged)", mdl ? mdl : "(unchanged)");
    return strdup(buf);
}

int tool_configure_provider_register(ToolRegistry *reg, ToolBootstrapCtx *ctx) {
    return tools_register(reg, "configure_provider",
                          "Set up LLM provider. Stores API key encrypted. "
                          "Known providers: openrouter, gemini, anthropic. "
                          "Use 'custom' with base_url for others.",
                          CONFIGURE_PROVIDER_PARAMS,
                          tool_configure_provider_handler, ctx);
}

/* ── T191: configure_channel ─────────────────────────────────────── */

static const char *CONFIGURE_CHANNEL_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"channel_type\":{\"type\":\"string\",\"description\":\"Channel type: telegram or cli\"},"
    "\"bot_token\":{\"type\":\"string\",\"description\":\"Bot token (required for telegram, omit for cli)\"}"
    "},\"required\":[\"channel_type\"]}";

static char *tool_configure_channel_handler(const char *arguments, void *user_data) {
    ToolBootstrapCtx *ctx = (ToolBootstrapCtx *)user_data;
    if (!ctx || !ctx->db)
        return strdup("error: configure_channel unavailable");

    cJSON *json = cJSON_Parse(arguments);
    if (!json) return strdup("error: invalid JSON arguments");

    cJSON *channel_type = cJSON_GetObjectItemCaseSensitive(json, "channel_type");
    cJSON *bot_token = cJSON_GetObjectItemCaseSensitive(json, "bot_token");

    if (!cJSON_IsString(channel_type) || !channel_type->valuestring[0]) {
        cJSON_Delete(json);
        return strdup("error: 'channel_type' is required (telegram or cli)");
    }

    const char *ctype = channel_type->valuestring;

    if (strcmp(ctype, "telegram") == 0) {
        if (!cJSON_IsString(bot_token) || !bot_token->valuestring[0]) {
            cJSON_Delete(json);
            return strdup("error: 'bot_token' is required for telegram channel");
        }
        /* V67: Store bot token encrypted */
        if (db_kv_set_secret(ctx->db, "telegram_token", bot_token->valuestring) != 0) {
            cJSON_Delete(json);
            return strdup("error: failed to store bot token");
        }
        cJSON_Delete(json);
        return strdup(SENTINEL_CONFIG "channel configured: telegram");
    } else if (strcmp(ctype, "cli") == 0) {
        /* CLI needs no credentials — just acknowledge */
        cJSON_Delete(json);
        return strdup(SENTINEL_CONFIG "channel configured: cli");
    } else {
        cJSON_Delete(json);
        return strdup("error: unknown channel_type — use 'telegram' or 'cli'");
    }
}

int tool_configure_channel_register(ToolRegistry *reg, ToolBootstrapCtx *ctx) {
    return tools_register(reg, "configure_channel",
                          "Set up a communication channel. "
                          "Supported: telegram (requires bot_token), cli (no credentials).",
                          CONFIGURE_CHANNEL_PARAMS,
                          tool_configure_channel_handler, ctx);
}

/* ── T192: create_agent ──────────────────────────────────────────── */

static const char *CREATE_AGENT_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\",\"description\":\"Agent name (alphanumeric + hyphens)\"},"
    "\"model\":{\"type\":\"string\",\"description\":\"LLM model identifier\"},"
    "\"system_prompt\":{\"type\":\"string\",\"description\":\"Agent persona/system prompt\"},"
    "\"tools\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Tool whitelist\"},"
    "\"allowed_hosts\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Allowed network hosts\"}"
    "},\"required\":[\"name\"]}";

static char *tool_create_agent_handler(const char *arguments, void *user_data) {
    ToolBootstrapCtx *ctx = (ToolBootstrapCtx *)user_data;
    if (!ctx || !ctx->db)
        return strdup("error: create_agent unavailable");

    cJSON *json = cJSON_Parse(arguments);
    if (!json) return strdup("error: invalid JSON arguments");

    cJSON *name = cJSON_GetObjectItemCaseSensitive(json, "name");
    if (!cJSON_IsString(name) || !name->valuestring[0]) {
        cJSON_Delete(json);
        return strdup("error: 'name' is required");
    }

    /* Reject invalid names */
    const char *n = name->valuestring;
    if (strchr(n, '/') || strchr(n, '\\') || strcmp(n, "..") == 0) {
        cJSON_Delete(json);
        return strdup("error: invalid agent name (no path separators)");
    }

    /* Build payload for approval */
    char *payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!payload) return strdup("error: failed to serialize payload");

    int64_t id = approval_insert(ctx->db, ctx->session_id,
                                 ctx->agent_name ? ctx->agent_name : "bootstrap",
                                 "create_agent", payload);
    free(payload);

    if (id < 0)
        return strdup("error: failed to submit agent creation request");

    char buf[128];
    snprintf(buf, sizeof(buf), SENTINEL_CONFIG "create_agent approval #%lld", (long long)id);
    return strdup(buf);
}

int tool_create_agent_register(ToolRegistry *reg, ToolBootstrapCtx *ctx) {
    return tools_register(reg, "create_agent",
                          "Propose creation of a named agent. Requires admin approval. "
                          "On approval, daemon creates agent directory, seeds DB, and binds to channel.",
                          CREATE_AGENT_PARAMS,
                          tool_create_agent_handler, ctx);
}
