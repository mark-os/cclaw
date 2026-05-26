#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "db.h"
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
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len <= 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* Write string to file only if file does not exist. */
static void write_file_if_missing(const char *path, const char *content) {
    FILE *f = fopen(path, "r");
    if (f) { fclose(f); return; }
    f = fopen(path, "w");
    if (!f) return;
    fputs(content, f);
    fclose(f);
}

static const char *DEFAULT_SOUL = TPL_DEFAULT_SOUL_MD;

static const char *DEFAULT_MEMORY = TPL_DEFAULT_MEMORY_MD;

static const char *DEFAULT_SYSTEM_PROMPT = TPL_DEFAULT_SYSTEM_PROMPT_MD;

int workspace_init(const Config *cfg) {
    if (!cfg || !cfg->workspace) return -1;
    /* mkdir -p workspace */
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", cfg->workspace);
    if (system(cmd) != 0) return -1;
    /* Populate default files */
    char path[4096];
    snprintf(path, sizeof(path), "%s/SOUL.md", cfg->workspace);
    write_file_if_missing(path, DEFAULT_SOUL);
    snprintf(path, sizeof(path), "%s/MEMORY.md", cfg->workspace);
    write_file_if_missing(path, DEFAULT_MEMORY);
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

    /* Read workspace context files */
    char soul_path[4096], mem_path[4096];
    char *soul = NULL, *memory = NULL;
    if (cfg->workspace) {
        snprintf(soul_path, sizeof(soul_path), "%s/SOUL.md", cfg->workspace);
        snprintf(mem_path, sizeof(mem_path), "%s/MEMORY.md", cfg->workspace);
        soul = read_file(soul_path);
        memory = read_file(mem_path);
    }

    /* Build output with template expansion + context files appended */
    size_t tmpl_len = strlen(tmpl);
    size_t soul_len = soul ? strlen(soul) : 0;
    size_t mem_len = memory ? strlen(memory) : 0;
    size_t out_cap = tmpl_len + soul_len + mem_len + 256;
    char *out = malloc(out_cap);
    if (!out) { free(soul); free(memory); return str_dup(tmpl); }

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

    /* Append SOUL.md content */
    if (soul && soul_len > 0) {
        const char *hdr = "\n\n## SOUL.md\n";
        size_t hlen = strlen(hdr);
        while (oi + hlen + soul_len >= out_cap) { out_cap *= 2; out = realloc(out, out_cap); }
        memcpy(out + oi, hdr, hlen); oi += hlen;
        memcpy(out + oi, soul, soul_len); oi += soul_len;
    }

    /* Append MEMORY.md content */
    if (memory && mem_len > 0) {
        const char *hdr = "\n\n## MEMORY.md\n";
        size_t hlen = strlen(hdr);
        while (oi + hlen + mem_len >= out_cap) { out_cap *= 2; out = realloc(out, out_cap); }
        memcpy(out + oi, hdr, hlen); oi += hlen;
        memcpy(out + oi, memory, mem_len); oi += mem_len;
    }

    out[oi] = '\0';
    free(soul);
    free(memory);
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

/* V61,T170: Build Config struct from kv table + env var overrides.
 * Priority: env var > kv value > hardcoded default. */
Config *config_load_from_kv(sqlite3 *db) {
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

    /* api_key uses secret-aware getter */
    cfg->provider.api_key = db_kv_get_secret(db, "provider.api_key");

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
            else cfg->provider.cache_hints = CACHE_HINTS_AUTO;
            free(v);
        } else {
            cfg->provider.cache_hints = CACHE_HINTS_AUTO;
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

    KV_STR("telegram_token", "");
    cfg->telegram_token = cfg_val;

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

    KV_INT("max_iterations", 25);
    cfg->max_iterations = int_val;

    KV_INT("max_history_tokens", 0);
    cfg->max_history_tokens = int_val;

    KV_INT("heartbeat_interval", 0);
    cfg->heartbeat_interval = int_val;

    KV_INT("shell_timeout", 30);
    cfg->shell_timeout = int_val;

    KV_INT("stale_lock_timeout", 300);
    cfg->stale_lock_timeout = int_val;

    #undef KV_STR
    #undef KV_INT

    /* Env var overrides (highest priority per V61) */
    env_override_str(&cfg->provider.api_key, "OPENROUTER_API_KEY");
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

    return cfg;
}
