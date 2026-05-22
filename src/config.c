#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

static char *str_dup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *d = malloc(len);
    if (d) memcpy(d, s, len);
    return d;
}

static char *json_str(cJSON *obj, const char *key) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring) return str_dup(v->valuestring);
    return NULL;
}

static int json_int(cJSON *obj, const char *key, int fallback) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(v)) return v->valueint;
    return fallback;
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

Config *config_load(const char *path) {
    Config *cfg = calloc(1, sizeof(Config));
    if (!cfg) return NULL;

    /* Defaults */
    cfg->provider.base_url = str_dup("https://openrouter.ai/api/v1");
    cfg->provider.model = str_dup("deepseek/deepseek-v4-flash");
    cfg->provider.max_tokens = 4096;
    cfg->provider.context_window = 65536;
    cfg->db_path = str_dup("build/cclaw.db");
    cfg->workspace = str_dup("./workspace");
    cfg->web_port = 8080;
    cfg->max_iterations = 25;

    /* Parse JSON file if provided */
    if (path) {
        FILE *f = fopen(path, "rb");
        if (!f) { config_free(cfg); return NULL; }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = malloc((size_t)len + 1);
        if (!buf) { fclose(f); config_free(cfg); return NULL; }
        fread(buf, 1, (size_t)len, f);
        buf[len] = '\0';
        fclose(f);

        cJSON *root = cJSON_Parse(buf);
        free(buf);
        if (!root) { config_free(cfg); return NULL; }

        cJSON *prov = cJSON_GetObjectItemCaseSensitive(root, "provider");
        if (prov) {
            char *s;
            s = json_str(prov, "base_url");
            if (s) { free(cfg->provider.base_url); cfg->provider.base_url = s; }
            s = json_str(prov, "api_key");
            if (s) { free(cfg->provider.api_key); cfg->provider.api_key = s; }
            s = json_str(prov, "model");
            if (s) { free(cfg->provider.model); cfg->provider.model = s; }
            cfg->provider.max_tokens = json_int(prov, "max_tokens", cfg->provider.max_tokens);
            cfg->provider.context_window = json_int(prov, "context_window", cfg->provider.context_window);
        }

        /* T45: parse fallback providers array */
        cJSON *providers = cJSON_GetObjectItemCaseSensitive(root, "providers");
        if (providers && cJSON_IsArray(providers)) {
            int count = cJSON_GetArraySize(providers);
            if (count > 0) {
                cfg->fallback_providers = calloc((size_t)count, sizeof(ProviderConfig));
                if (cfg->fallback_providers) {
                    cfg->fallback_count = (size_t)count;
                    for (int i = 0; i < count; i++) {
                        cJSON *p = cJSON_GetArrayItem(providers, i);
                        cfg->fallback_providers[i].base_url = json_str(p, "base_url");
                        cfg->fallback_providers[i].api_key = json_str(p, "api_key");
                        cfg->fallback_providers[i].model = json_str(p, "model");
                        cfg->fallback_providers[i].max_tokens = json_int(p, "max_tokens", cfg->provider.max_tokens);
                        cfg->fallback_providers[i].context_window = json_int(p, "context_window", cfg->provider.context_window);
                    }
                }
            }
        }

        char *s;
        s = json_str(root, "db_path");
        if (s) { free(cfg->db_path); cfg->db_path = s; }
        s = json_str(root, "workspace");
        if (s) { free(cfg->workspace); cfg->workspace = s; }
        s = json_str(root, "telegram_token");
        if (s) { free(cfg->telegram_token); cfg->telegram_token = s; }
        s = json_str(root, "system_prompt");
        if (s) { free(cfg->system_prompt); cfg->system_prompt = s; }
        cfg->web_port = json_int(root, "web_port", cfg->web_port);
        cfg->max_iterations = json_int(root, "max_iterations", cfg->max_iterations);
        cfg->max_history_tokens = json_int(root, "max_history_tokens", cfg->max_history_tokens);
        cfg->heartbeat_interval = json_int(root, "heartbeat_interval", cfg->heartbeat_interval);
        cfg->shell_timeout = json_int(root, "shell_timeout", cfg->shell_timeout);
        cfg->stale_lock_timeout = json_int(root, "stale_lock_timeout", cfg->stale_lock_timeout);

        cJSON_Delete(root);
    }

    /* Env var overrides (highest priority) */
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

    return cfg;
}

/* T46: render system prompt with template vars */
char *config_render_system_prompt(const Config *cfg, int64_t session_id) {
    const char *tmpl = cfg->system_prompt
        ? cfg->system_prompt
        : "You are CClaw, a helpful AI assistant.";

    /* Build date string */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char date_buf[11]; /* YYYY-MM-DD */
    strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm);

    /* Build session_id string */
    char sid_buf[21];
    snprintf(sid_buf, sizeof(sid_buf), "%lld", (long long)session_id);

    /* Replace {session_id} and {date} */
    size_t tmpl_len = strlen(tmpl);
    size_t out_cap = tmpl_len + 64;
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
    free(cfg->system_prompt);
    free(cfg);
}
