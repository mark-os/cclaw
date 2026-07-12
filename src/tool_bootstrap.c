#define _POSIX_C_SOURCE 200809L
#include "tool_bootstrap.h"
#include "agent_define.h"
#include "approval.h"
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

/* EXEC_THREAD shim: rebuild a minimal ctx around the thread's live db handle.
 * configure_provider applies directly (providers upsert + encrypted kv). */
static char *configure_provider_thread_run(sqlite3 *db, const char *agent_name,
                                           int64_t session_id, const char *args) {
    ToolBootstrapCtx c = {.db = db, .session_id = session_id, .agent_name = agent_name};
    return tool_configure_provider_handler(args, &c);
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

/* ── create_agent ─────────────────────────────────────────────────── */

static const char *CREATE_AGENT_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\",\"description\":\"Agent name (PascalCase: uppercase first letter, letters and digits only)\"},"
    "\"description\":{\"type\":\"string\",\"description\":\"One-line hint: when to delegate to this agent\"},"
    "\"system_prompt\":{\"type\":\"string\",\"description\":\"Agent persona/system prompt\"},"
    "\"primary_model\":{\"type\":\"string\",\"description\":\"Primary model (omit for default)\"},"
    "\"secondary_model\":{\"type\":\"string\",\"description\":\"Fallback model (omit for default)\"},"
    "\"sandbox_profile\":{\"type\":\"string\",\"enum\":[\"host\",\"trusted\",\"standard\",\"restricted\"],\"description\":\"Containment profile; must not be looser than yours\"},"
    "\"grants\":{\"type\":\"object\",\"properties\":{"
      "\"tools\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
      "\"hosts\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
      "\"read_paths\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
      "\"write_paths\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},"
      "\"description\":\"Grants beyond the baseline; each must be a grant you hold\"},"
    "\"extensions\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Extensions to attach (published, or owned by you)\"},"
    "\"memory_blocks\":{\"type\":\"array\",\"items\":{\"type\":\"object\"},\"description\":\"Memory blocks: {label, description, value, char_limit?, read_only?, placement?}\"},"
    "\"max_iterations\":{\"type\":\"integer\"},"
    "\"shell_timeout\":{\"type\":\"integer\"},"
    "\"clone_from\":{\"type\":\"string\",\"description\":\"Copy an existing agent's config/grants/extensions first, then overlay\"}"
    "},\"required\":[\"name\"]}";

static char *tool_create_agent_handler(const char *arguments, void *user_data) {
    ToolBootstrapCtx *ctx = (ToolBootstrapCtx *)user_data;
    if (!ctx || !ctx->db)
        return strdup("error: create_agent unavailable");

    /* Eager validation with creator caps — a definition that can't apply
     * never parks. */
    char *err = NULL;
    if (agent_definition_validate(ctx->db, arguments, ctx->agent_name, &err) != 0) {
        char *msg;
        if (err) {
            size_t n = strlen(err) + 8;
            msg = malloc(n);
            if (msg) snprintf(msg, n, "error: %s", err);
            free(err);
        } else {
            msg = strdup("error: invalid agent definition");
        }
        return msg ? msg : strdup("error: invalid agent definition");
    }

    int64_t aid = approval_create(ctx->db, ctx->session_id,
        ctx->current_tool_call_id, "create_agent", "create_agent",
        arguments, "apply");
    if (aid < 0)
        return strdup("error: failed to create approval");
    session_set_state(ctx->db, ctx->session_id, "awaiting_approval");
    return NULL; /* park */
}

int tool_create_agent_register(ToolRegistry *reg, ToolBootstrapCtx *ctx) {
    int rc = tools_register(reg, "create_agent",
                          "Propose creation of a named agent (requires human approval). "
                          "Takes an agent definition: description, system_prompt, models, "
                          "sandbox_profile, grants, extensions, memory_blocks, or clone_from. "
                          "The new agent is capped by your own profile and grants.",
                          CREATE_AGENT_PARAMS,
                          tool_create_agent_handler, ctx);
    if (rc == 0)
        tools_set_recipe(reg, "create_agent",
                         (ToolRecipe){EXEC_INLINE, SBX_NONE, NULL});
    return rc;
}
