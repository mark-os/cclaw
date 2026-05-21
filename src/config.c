#include "config.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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
    cfg->db_path = str_dup("cclaw.db");
    cfg->workspace = str_dup("./workspace");
    cfg->web_port = 8080;

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

        char *s;
        s = json_str(root, "db_path");
        if (s) { free(cfg->db_path); cfg->db_path = s; }
        s = json_str(root, "workspace");
        if (s) { free(cfg->workspace); cfg->workspace = s; }
        s = json_str(root, "telegram_token");
        if (s) { free(cfg->telegram_token); cfg->telegram_token = s; }
        cfg->web_port = json_int(root, "web_port", cfg->web_port);

        cJSON_Delete(root);
    }

    /* Env var overrides (highest priority) */
    env_override_str(&cfg->provider.api_key, "OPENROUTER_API_KEY");
    env_override_str(&cfg->provider.base_url, "CCLAW_PROVIDER");
    env_override_str(&cfg->provider.model, "CCLAW_MODEL");
    env_override_str(&cfg->telegram_token, "CCLAW_TELEGRAM_TOKEN");
    env_override_str(&cfg->db_path, "CCLAW_DB_PATH");
    env_override_int(&cfg->web_port, "CCLAW_WEB_PORT");

    return cfg;
}

void config_free(Config *cfg) {
    if (!cfg) return;
    free(cfg->provider.base_url);
    free(cfg->provider.api_key);
    free(cfg->provider.model);
    free(cfg->db_path);
    free(cfg->workspace);
    free(cfg->telegram_token);
    free(cfg);
}
