#define _POSIX_C_SOURCE 200809L
#include "tool_bootstrap.h"
#include "db.h"
#include <cJSON.h>
#include "tool_parse.h"
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
    if (!ctx)
        return strdup("error: configure_provider unavailable");

    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0)
        return strdup("error: invalid JSON arguments");

    const char *pname = targ_str(&ta, "provider");
    const char *api_key = targ_str(&ta, "api_key");
    const char *base_url = targ_str(&ta, "base_url");

    if (!pname || !api_key || !api_key[0]) {
        tool_parse_free(&ta);
        return strdup("error: 'provider' and 'api_key' are required");
    }

    /* Validate known provider or custom with base_url */
    int known = 0;
    for (size_t i = 0; i < PROVIDER_COUNT; i++) {
        if (strcmp(pname, PROVIDERS[i].name) == 0) { known = 1; break; }
    }
    if (!known && (!base_url || !base_url[0])) {
        tool_parse_free(&ta);
        return strdup("error: 'base_url' is required for custom providers");
    }

    tool_parse_free(&ta);

    /* V76/V79: Don't write cclaw.db — return sentinel, daemon applies on reap */
    return strdup("config applied: configure_provider");
}

int tool_configure_provider_register(ToolRegistry *reg, ToolBootstrapCtx *ctx) {
    return tools_register(reg, "configure_provider",
                          "Set up LLM provider. Stores API key encrypted. "
                          "Known providers: openrouter, gemini, anthropic. "
                          "Use 'custom' with base_url for others.",
                          CONFIGURE_PROVIDER_PARAMS,
                          tool_configure_provider_handler, ctx);
}

/* ── T251: configure_channel ─────────────────────────────────────── */

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

    /* V76/V79: Don't write cclaw.db — return sentinel, daemon applies on reap */
    return strdup("config applied: configure_channel");
}

int tool_configure_channel_register(ToolRegistry *reg, ToolBootstrapCtx *ctx) {
    return tools_register(reg, "configure_channel",
                          "Set up a communication channel. "
                          "Supported: telegram (requires bot_token), cli, or custom (requires binary_path). "
                          "Optional config object seeds channel_state kv pairs.",
                          CONFIGURE_CHANNEL_PARAMS,
                          tool_configure_channel_handler, ctx);
}

/* ── T192: create_agent ──────────────────────────────────────────── */

static const char *CREATE_AGENT_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\",\"description\":\"Agent name (alphanumeric + hyphens)\"},"
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
    if (strchr(n, '/') || strchr(n, '\\') || strcmp(n, "..") == 0) {
        tool_parse_free(&ta);
        return strdup("error: invalid agent name (no path separators)");
    }

    tool_parse_free(&ta);

    /* T201/V79: Return sentinel — daemon reads args from tool_call entry */
    return strdup("config applied: create_agent");
}

int tool_create_agent_register(ToolRegistry *reg, ToolBootstrapCtx *ctx) {
    return tools_register(reg, "create_agent",
                          "Propose creation of a named agent. Requires admin approval. "
                          "On approval, daemon creates agent directory, seeds DB, and binds to channel.",
                          CREATE_AGENT_PARAMS,
                          tool_create_agent_handler, ctx);
}
