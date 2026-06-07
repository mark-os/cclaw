#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "agent_config.h"
#include "db.h"
#include "log.h"
#include "cJSON.h"
#include "templates.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>

static char *str_dup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *d = malloc(len);
    if (d) memcpy(d, s, len);
    return d;
}

/* Apply env var override: if env set, replace *field */
static void env_override_str(char **field, const char *env_name) {
    const char *val = getenv(env_name);
    if (val) {
        free(*field);
        *field = str_dup(val);
    }
}

static void env_override_int(int *field, const char *env_name) {
    const char *val = getenv(env_name);
    if (val) *field = atoi(val);
}

/* Read file into malloc'd string. Returns NULL if file doesn't exist or on error. */
static const char *DEFAULT_SYSTEM_PROMPT = TPL_DEFAULT_SYSTEM_PROMPT_MD;

int workspace_init(const Config *cfg) {
    if (!cfg || !cfg->workspace) return -1;
    /* mkdir -p workspace */
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", cfg->workspace);
    if (system(cmd) != 0) return -1;
    return 0;
}

/* T46: render system prompt with template vars and workspace context */
char *config_render_system_prompt(const Config *cfg, int64_t session_id) {
    const char *tmpl = cfg->system_prompt ? cfg->system_prompt : DEFAULT_SYSTEM_PROMPT;

    /* Build date string */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char date_buf[11];
    strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm);

    /* Build session_id string */
    char sid_buf[21];
    snprintf(sid_buf, sizeof(sid_buf), "%lld", (long long)session_id);

    /* Build output with template expansion */
    size_t tmpl_len = strlen(tmpl);
    size_t out_cap = tmpl_len + 256;
    char *out = malloc(out_cap);
    if (!out) return str_dup(tmpl);

    size_t oi = 0;
    for (size_t i = 0; i < tmpl_len; ) {
        if (tmpl[i] == '{') {
            if (strncmp(tmpl + i, "{session_id}", 12) == 0) {
                size_t slen = strlen(sid_buf);
                while (oi + slen >= out_cap) { out_cap *= 2; out = realloc(out, out_cap); }
                memcpy(out + oi, sid_buf, slen);
                oi += slen;
                i += 12;
                continue;
            }
            if (strncmp(tmpl + i, "{date}", 6) == 0) {
                size_t dlen = strlen(date_buf);
                while (oi + dlen >= out_cap) { out_cap *= 2; out = realloc(out, out_cap); }
                memcpy(out + oi, date_buf, dlen);
                oi += dlen;
                i += 6;
                continue;
            }
            if (strncmp(tmpl + i, "{workspace}", 11) == 0) {
                const char *ws = cfg->workspace ? cfg->workspace : ".";
                size_t wlen = strlen(ws);
                while (oi + wlen >= out_cap) { out_cap *= 2; out = realloc(out, out_cap); }
                memcpy(out + oi, ws, wlen);
                oi += wlen;
                i += 11;
                continue;
            }
        }
        if (oi + 1 >= out_cap) { out_cap *= 2; out = realloc(out, out_cap); }
        out[oi++] = tmpl[i++];
    }

    out[oi] = '\0';
    return out;
}

void config_free(Config *cfg) {
    if (!cfg) return;
    free(cfg->provider.base_url);
    free(cfg->provider.api_key);
    free(cfg->provider.model);
    for (size_t i = 0; i < cfg->fallback_count; i++) {
        free(cfg->fallback_providers[i].base_url);
        free(cfg->fallback_providers[i].api_key);
        free(cfg->fallback_providers[i].model);
    }
    free(cfg->fallback_providers);
    free(cfg->db_path);
    free(cfg->workspace);
    free(cfg->telegram_token);
    free(cfg->admin_chat_ids);
    free(cfg->system_prompt);
    free(cfg->env_file);
    free(cfg);
}

/* V74,T198: Build Config purely from CCLAW_* env vars.
 * Agent process path — daemon injects all config at fork. */
Config *config_load_from_env(void) {
    Config *cfg = calloc(1, sizeof(Config));
    if (!cfg) return NULL;

    const char *v;

    /* Provider */
    v = getenv("CCLAW_PROVIDER_API_KEY_ENV");
    if (v && v[0]) {
        const char *key = getenv(v);
        cfg->provider.api_key = str_dup(key ? key : "");
    } else {
        v = getenv("OPENROUTER_API_KEY");
        cfg->provider.api_key = str_dup(v ? v : "");
    }

    v = getenv("CCLAW_PROVIDER_BASE_URL");
    if (!v || !v[0]) v = getenv("CCLAW_PROVIDER");
    cfg->provider.base_url = str_dup(v ? v : "https://openrouter.ai/api/v1");

    v = getenv("CCLAW_MODEL");
    cfg->provider.model = str_dup(v ? v : "deepseek/deepseek-v4-flash");

    v = getenv("CCLAW_MAX_TOKENS");
    cfg->provider.max_tokens = v ? atoi(v) : 4096;

    v = getenv("CCLAW_CONTEXT_WINDOW");
    cfg->provider.context_window = v ? atoi(v) : 65536;

    /* T290: endpoint type */
    v = getenv("CCLAW_PROVIDER_ENDPOINT_TYPE");
    if (v && strcmp(v, "gemini") == 0)
        cfg->provider.endpoint_type = ENDPOINT_GEMINI;
    else
        cfg->provider.endpoint_type = ENDPOINT_OPENAI;

    /* T223: Workspace — default under .cclaw/agents/default/ for zero-config CLI */
    v = getenv("CCLAW_WORKSPACE");
    cfg->workspace = str_dup(v ? v : ".cclaw/agents/default/workspace");

    /* DB path — parent sets CCLAW_DB for children at fork */
    v = getenv("CCLAW_DB");
    cfg->db_path = str_dup(v ? v : "cclaw.db");

    /* Scalars */
    v = getenv("CCLAW_MAX_ITERATIONS");
    cfg->max_iterations = v ? atoi(v) : AGENT_DEFAULT_MAX_ITERATIONS;

    v = getenv("CCLAW_MAX_HISTORY_TOKENS");
    cfg->max_history_tokens = v ? atoi(v) : 0;

    v = getenv("CCLAW_SHELL_TIMEOUT");
    cfg->shell_timeout = v ? atoi(v) : AGENT_DEFAULT_SHELL_TIMEOUT;

    v = getenv("CCLAW_TOKEN_RATE_LIMIT");
    cfg->token_rate_limit = v ? atoi(v) : 1000000;

    v = getenv("CCLAW_SAVE_REASONING");
    cfg->save_reasoning = v ? atoi(v) : 0;

    v = getenv("CCLAW_SAVE_USAGE");
    cfg->save_usage = v ? atoi(v) : 0;

    v = getenv("CCLAW_SAVE_LOGPROBS");
    cfg->save_logprobs = v ? atoi(v) : 0;

    v = getenv("CCLAW_LOG_LEVEL");
    cfg->log_level = log_level_parse(v);

    v = getenv("CCLAW_CONTEXT_THRESHOLD");
    cfg->context_threshold = v ? (float)atof(v) : 0.6f;

    v = getenv("CCLAW_COMPACTION_TARGET");
    cfg->compaction_target = v ? (float)atof(v) : 0.3f;

    v = getenv("CCLAW_COMPACTION");
    cfg->compaction = v ? atoi(v) : 1;

    v = getenv("CCLAW_AUTO_RECALL");
    cfg->auto_recall = v ? atoi(v) : 1;

    v = getenv("CCLAW_RECALL_MAX_TOKENS");
    cfg->recall_max_tokens = v ? atoi(v) : 500;

    v = getenv("CCLAW_STREAM");
    cfg->stream = v ? atoi(v) : 0;

    /* T292: cache_hints */
    v = getenv("CCLAW_CACHE_HINTS");
    if (v) {
        if (strcmp(v, "on") == 0) cfg->provider.cache_hints = CACHE_HINTS_ON;
        else if (strcmp(v, "off") == 0) cfg->provider.cache_hints = CACHE_HINTS_OFF;
        else if (strcmp(v, "gemini-native") == 0) cfg->provider.cache_hints = CACHE_HINTS_GEMINI_NATIVE;
        else cfg->provider.cache_hints = CACHE_HINTS_AUTO;
    }

    return cfg;
}

/* Build Config for parent processes (CLI/daemon).
 * Priority: env var > kv value > hardcoded default. */
Config *config_load(sqlite3 *db) {
    if (!db) return NULL;

    Config *cfg = calloc(1, sizeof(Config));
    if (!cfg) return NULL;

    /* Helper: read kv string, fallback to default */
    #define KV_STR(key, def) do { \
        char *v = db_kv_get(db, key); \
        if (v) { cfg_val = v; } else { cfg_val = str_dup(def); } \
    } while(0)

    #define KV_INT(key, def) do { \
        char *v = db_kv_get(db, key); \
        int_val = v ? atoi(v) : (def); \
        free(v); \
    } while(0)

    char *cfg_val;
    int int_val;

    /* Provider config */
    KV_STR("provider.base_url", "https://openrouter.ai/api/v1");
    cfg->provider.base_url = cfg_val;

    KV_STR("provider.model", "deepseek/deepseek-v4-flash");
    cfg->provider.model = cfg_val;

    /* api_key: V67/T188 — prefer env var (from provider's api_key_env) over DB decryption */
    {
        const char *key_env = getenv("CCLAW_PROVIDER_API_KEY_ENV");
        const char *injected = key_env && key_env[0] ? getenv(key_env) : NULL;
        if (injected && injected[0])
            cfg->provider.api_key = str_dup(injected);
        else
            cfg->provider.api_key = db_kv_get_secret(db, "provider.api_key");
    }

    KV_INT("provider.max_tokens", 4096);
    cfg->provider.max_tokens = int_val;

    KV_INT("provider.context_window", 65536);
    cfg->provider.context_window = int_val;

    /* cache_hints */
    {
        char *v = db_kv_get(db, "provider.cache_hints");
        if (v) {
            if (strcmp(v, "on") == 0) cfg->provider.cache_hints = CACHE_HINTS_ON;
            else if (strcmp(v, "off") == 0) cfg->provider.cache_hints = CACHE_HINTS_OFF;
            else if (strcmp(v, "gemini-native") == 0) cfg->provider.cache_hints = CACHE_HINTS_GEMINI_NATIVE;
            else cfg->provider.cache_hints = CACHE_HINTS_AUTO;
            free(v);
        } else {
            cfg->provider.cache_hints = CACHE_HINTS_AUTO;
        }
    }

    /* T290: endpoint_type */
    {
        char *v = db_kv_get(db, "provider.endpoint_type");
        if (v) {
            if (strcmp(v, "gemini") == 0) cfg->provider.endpoint_type = ENDPOINT_GEMINI;
            else cfg->provider.endpoint_type = ENDPOINT_OPENAI;
            free(v);
        } else {
            cfg->provider.endpoint_type = ENDPOINT_OPENAI;
        }
    }

    /* Fallback providers (JSON array in kv) */
    {
        char *fp = db_kv_get(db, "fallback_providers");
        if (fp && fp[0] != '\0' && strcmp(fp, "[]") != 0) {
            cJSON *arr = cJSON_Parse(fp);
            if (arr && cJSON_IsArray(arr)) {
                int count = cJSON_GetArraySize(arr);
                if (count > 0) {
                    cfg->fallback_providers = calloc((size_t)count, sizeof(ProviderConfig));
                    if (cfg->fallback_providers) {
                        cfg->fallback_count = (size_t)count;
                        for (int i = 0; i < count; i++) {
                            cJSON *p = cJSON_GetArrayItem(arr, i);
                            cJSON *v;
                            v = cJSON_GetObjectItemCaseSensitive(p, "base_url");
                            if (cJSON_IsString(v)) cfg->fallback_providers[i].base_url = str_dup(v->valuestring);
                            v = cJSON_GetObjectItemCaseSensitive(p, "api_key");
                            if (cJSON_IsString(v)) cfg->fallback_providers[i].api_key = str_dup(v->valuestring);
                            v = cJSON_GetObjectItemCaseSensitive(p, "model");
                            if (cJSON_IsString(v)) cfg->fallback_providers[i].model = str_dup(v->valuestring);
                            v = cJSON_GetObjectItemCaseSensitive(p, "max_tokens");
                            cfg->fallback_providers[i].max_tokens = cJSON_IsNumber(v) ? v->valueint : cfg->provider.max_tokens;
                            v = cJSON_GetObjectItemCaseSensitive(p, "context_window");
                            cfg->fallback_providers[i].context_window = cJSON_IsNumber(v) ? v->valueint : cfg->provider.context_window;
                        }
                    }
                }
            }
            cJSON_Delete(arr);
        }
        free(fp);
    }

    /* DB path: derive from DB handle's filename */
    {
        const char *db_filename = sqlite3_db_filename(db, "main");
        if (db_filename && db_filename[0])
            cfg->db_path = str_dup(db_filename);
        else
            cfg->db_path = str_dup("cclaw.db");
    }

    KV_STR("workspace", "./workspace");
    cfg->workspace = cfg_val;

    /* telegram_token: V67/T188 — prefer daemon-injected env var over DB */
    {
        const char *injected = getenv("CCLAW_INJECTED_TELEGRAM_TOKEN");
        if (injected && injected[0])
            cfg->telegram_token = str_dup(injected);
        else {
            char *v = db_kv_get_secret(db, "telegram_token");
            cfg->telegram_token = v ? v : str_dup("");
        }
    }

    /* admin_chat_ids (JSON array in kv) */
    {
        char *ids = db_kv_get(db, "admin_chat_ids");
        if (ids && ids[0] != '\0' && strcmp(ids, "[]") != 0) {
            cJSON *arr = cJSON_Parse(ids);
            if (arr && cJSON_IsArray(arr)) {
                int n = cJSON_GetArraySize(arr);
                if (n > 0) {
                    cfg->admin_chat_ids = calloc((size_t)n, sizeof(int64_t));
                    if (cfg->admin_chat_ids) {
                        cfg->admin_chat_id_count = (size_t)n;
                        for (int i = 0; i < n; i++) {
                            cJSON *item = cJSON_GetArrayItem(arr, i);
                            if (cJSON_IsNumber(item))
                                cfg->admin_chat_ids[i] = (int64_t)item->valuedouble;
                        }
                    }
                }
            }
            cJSON_Delete(arr);
        }
        free(ids);
    }

    KV_INT("web_port", 8080);
    cfg->web_port = int_val;

    KV_INT("max_iterations", AGENT_DEFAULT_MAX_ITERATIONS);
    cfg->max_iterations = int_val;

    KV_INT("max_history_tokens", 0);
    cfg->max_history_tokens = int_val;

    KV_INT("heartbeat_interval", 0);
    cfg->heartbeat_interval = int_val;

    KV_INT("shell_timeout", AGENT_DEFAULT_SHELL_TIMEOUT);
    cfg->shell_timeout = int_val;

    KV_INT("stale_lock_timeout", 300);
    cfg->stale_lock_timeout = int_val;

    KV_INT("token_rate_limit", 1000000);
    cfg->token_rate_limit = int_val;

    /* V91: compaction configs */
    {
        char *v = db_kv_get(db, "context_threshold");
        cfg->context_threshold = v ? (float)atof(v) : 0.6f;
        free(v);
        v = db_kv_get(db, "compaction_target");
        cfg->compaction_target = v ? (float)atof(v) : 0.3f;
        free(v);
        v = db_kv_get(db, "compaction");
        cfg->compaction = v ? atoi(v) : 1;
        free(v);
    }

    #undef KV_STR
    #undef KV_INT

    /* Env var overrides (highest priority per V61) */
    env_override_str(&cfg->provider.api_key, "OPENROUTER_API_KEY");
    env_override_str(&cfg->provider.base_url, "CCLAW_PROVIDER_BASE_URL");
    env_override_str(&cfg->provider.base_url, "CCLAW_PROVIDER");
    env_override_str(&cfg->provider.model, "CCLAW_MODEL");
    env_override_str(&cfg->telegram_token, "CCLAW_TELEGRAM_TOKEN");
    env_override_str(&cfg->db_path, "CCLAW_DB_PATH");
    env_override_str(&cfg->system_prompt, "CCLAW_SYSTEM_PROMPT");
    env_override_int(&cfg->web_port, "CCLAW_WEB_PORT");
    env_override_int(&cfg->max_iterations, "CCLAW_MAX_ITERATIONS");
    env_override_int(&cfg->max_history_tokens, "CCLAW_MAX_HISTORY_TOKENS");
    env_override_int(&cfg->heartbeat_interval, "CCLAW_HEARTBEAT_INTERVAL");
    env_override_int(&cfg->shell_timeout, "CCLAW_SHELL_TIMEOUT");
    env_override_int(&cfg->stale_lock_timeout, "CCLAW_STALE_LOCK_TIMEOUT");
    env_override_int(&cfg->save_reasoning, "CCLAW_SAVE_REASONING");
    env_override_int(&cfg->save_usage, "CCLAW_SAVE_USAGE");
    env_override_int(&cfg->save_logprobs, "CCLAW_SAVE_LOGPROBS");
    env_override_int(&cfg->token_rate_limit, "CCLAW_TOKEN_RATE_LIMIT");

    /* T290: endpoint_type env override */
    {
        const char *v = getenv("CCLAW_PROVIDER_ENDPOINT_TYPE");
        if (v && strcmp(v, "gemini") == 0)
            cfg->provider.endpoint_type = ENDPOINT_GEMINI;
    }

    /* V91: compaction env overrides */
    {
        const char *v = getenv("CCLAW_CONTEXT_THRESHOLD");
        if (v) cfg->context_threshold = (float)atof(v);
        v = getenv("CCLAW_COMPACTION_TARGET");
        if (v) cfg->compaction_target = (float)atof(v);
        v = getenv("CCLAW_COMPACTION");
        if (v) cfg->compaction = atoi(v);
    }

    /* T269: auto-recall defaults + env overrides */
    if (cfg->auto_recall == 0) cfg->auto_recall = 1;
    if (cfg->recall_max_tokens == 0) cfg->recall_max_tokens = 500;
    env_override_int(&cfg->auto_recall, "CCLAW_AUTO_RECALL");
    env_override_int(&cfg->recall_max_tokens, "CCLAW_RECALL_MAX_TOKENS");

    return cfg;
}


