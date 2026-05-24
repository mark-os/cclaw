#define _POSIX_C_SOURCE 200809L
#include "agent_config.h"
#include "config.h"
#include "cJSON.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stdio.h>
#include <time.h>

char **agent_discover(const char *agents_dir, size_t *count) {
    *count = 0;
    DIR *d = opendir(agents_dir);
    if (!d) return NULL;

    size_t cap = 8;
    char **names = malloc(cap * sizeof(char *));
    if (!names) { closedir(d); return NULL; }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        /* Check it's a directory */
        char path[1024];
        int n = snprintf(path, sizeof(path), "%s/%s", agents_dir, ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) continue;

        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        if (*count >= cap) {
            cap *= 2;
            char **tmp = realloc(names, cap * sizeof(char *));
            if (!tmp) break;
            names = tmp;
        }
        names[*count] = strdup(ent->d_name);
        if (names[*count]) (*count)++;
    }
    closedir(d);
    return names;
}

void agent_discover_free(char **names, size_t count) {
    if (!names) return;
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
}

/* T76: load agent config from agents_dir/name/agent.json */
AgentConfig *agent_config_load(const char *agents_dir, const char *name) {
    if (!agents_dir || !name) return NULL;

    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/%s/agent.json", agents_dir, name);
    if (n < 0 || (size_t)n >= sizeof(path)) return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, (size_t)len, f);
    buf[len] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return NULL;

    AgentConfig *ac = calloc(1, sizeof(AgentConfig));
    if (!ac) { cJSON_Delete(root); return NULL; }

    ac->name = strdup(name);

    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "model");
    if (cJSON_IsString(v) && v->valuestring) ac->model = strdup(v->valuestring);

    v = cJSON_GetObjectItemCaseSensitive(root, "workspace");
    if (cJSON_IsString(v) && v->valuestring)
        ac->workspace = strdup(v->valuestring);

    v = cJSON_GetObjectItemCaseSensitive(root, "max_iterations");
    if (cJSON_IsNumber(v)) ac->max_iterations = v->valueint;

    /* Tool whitelist array */
    v = cJSON_GetObjectItemCaseSensitive(root, "tools");
    if (v && cJSON_IsArray(v)) {
        int cnt = cJSON_GetArraySize(v);
        if (cnt > 0) {
            ac->tools = malloc((size_t)cnt * sizeof(char *));
            if (ac->tools) {
                for (int i = 0; i < cnt; i++) {
                    cJSON *item = cJSON_GetArrayItem(v, i);
                    if (cJSON_IsString(item) && item->valuestring)
                        ac->tools[ac->tool_count++] = strdup(item->valuestring);
                }
            }
        }
    }

    /* Allowed hosts array */
    v = cJSON_GetObjectItemCaseSensitive(root, "allowed_hosts");
    if (v && cJSON_IsArray(v)) {
        int cnt = cJSON_GetArraySize(v);
        if (cnt > 0) {
            ac->allowed_hosts = malloc((size_t)cnt * sizeof(char *));
            if (ac->allowed_hosts) {
                for (int i = 0; i < cnt; i++) {
                    cJSON *item = cJSON_GetArrayItem(v, i);
                    if (cJSON_IsString(item) && item->valuestring)
                        ac->allowed_hosts[ac->allowed_hosts_count++] = strdup(item->valuestring);
                }
            }
        }
    }

    cJSON_Delete(root);

    /* V12: workspace fallback to ./workspace/{name} */
    if (!ac->workspace) {
        char ws[1024];
        snprintf(ws, sizeof(ws), "./workspace/%s", name);
        ac->workspace = strdup(ws);
    }

    return ac;
}

void agent_config_free(AgentConfig *ac) {
    if (!ac) return;
    free(ac->name);
    free(ac->model);
    free(ac->workspace);
    for (size_t i = 0; i < ac->tool_count; i++) free(ac->tools[i]);
    free(ac->tools);
    for (size_t i = 0; i < ac->allowed_hosts_count; i++) free(ac->allowed_hosts[i]);
    free(ac->allowed_hosts);
    free(ac);
}

Config *agent_config_merge(const Config *global, const AgentConfig *ac) {
    if (!global) return NULL;

    Config *cfg = calloc(1, sizeof(Config));
    if (!cfg) return NULL;

    /* Copy global fields */
    cfg->provider.base_url = global->provider.base_url ? strdup(global->provider.base_url) : NULL;
    cfg->provider.api_key = global->provider.api_key ? strdup(global->provider.api_key) : NULL;
    cfg->provider.model = global->provider.model ? strdup(global->provider.model) : NULL;
    cfg->provider.max_tokens = global->provider.max_tokens;
    cfg->provider.context_window = global->provider.context_window;
    cfg->db_path = global->db_path ? strdup(global->db_path) : NULL;
    cfg->workspace = global->workspace ? strdup(global->workspace) : NULL;
    cfg->telegram_token = global->telegram_token ? strdup(global->telegram_token) : NULL;
    cfg->system_prompt = global->system_prompt ? strdup(global->system_prompt) : NULL;
    cfg->web_port = global->web_port;
    cfg->max_iterations = global->max_iterations;
    cfg->max_history_tokens = global->max_history_tokens;
    cfg->heartbeat_interval = global->heartbeat_interval;
    cfg->shell_timeout = global->shell_timeout;
    cfg->stale_lock_timeout = global->stale_lock_timeout;
    cfg->debug = global->debug;

    /* Copy fallback providers */
    if (global->fallback_count > 0 && global->fallback_providers) {
        cfg->fallback_providers = calloc(global->fallback_count, sizeof(ProviderConfig));
        if (cfg->fallback_providers) {
            cfg->fallback_count = global->fallback_count;
            for (size_t i = 0; i < global->fallback_count; i++) {
                cfg->fallback_providers[i].base_url = global->fallback_providers[i].base_url ? strdup(global->fallback_providers[i].base_url) : NULL;
                cfg->fallback_providers[i].api_key = global->fallback_providers[i].api_key ? strdup(global->fallback_providers[i].api_key) : NULL;
                cfg->fallback_providers[i].model = global->fallback_providers[i].model ? strdup(global->fallback_providers[i].model) : NULL;
                cfg->fallback_providers[i].max_tokens = global->fallback_providers[i].max_tokens;
                cfg->fallback_providers[i].context_window = global->fallback_providers[i].context_window;
            }
        }
    }

    /* Apply agent overrides */
    if (ac) {
        if (ac->model) {
            free(cfg->provider.model);
            cfg->provider.model = strdup(ac->model);
        }
        if (ac->workspace) {
            free(cfg->workspace);
            cfg->workspace = strdup(ac->workspace);
        }
        if (ac->max_iterations > 0) {
            cfg->max_iterations = ac->max_iterations;
        }
    }

    return cfg;
}

/* T77: load system.md from agent dir, render template vars */
char *agent_load_system_prompt(const char *agents_dir, const char *name,
                               int64_t session_id) {
    if (!agents_dir || !name) return NULL;

    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/%s/system.md", agents_dir, name);
    if (n < 0 || (size_t)n >= sizeof(path)) return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return NULL; }

    char *tmpl = malloc((size_t)len + 1);
    if (!tmpl) { fclose(f); return NULL; }
    fread(tmpl, 1, (size_t)len, f);
    tmpl[len] = '\0';
    fclose(f);

    /* Build replacement strings */
    char sid_buf[21];
    snprintf(sid_buf, sizeof(sid_buf), "%lld", (long long)session_id);

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char date_buf[11];
    strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm);

    /* Render template: replace {session_id}, {date}, {agent_name} */
    size_t tmpl_len = (size_t)len;
    size_t out_cap = tmpl_len + 128;
    char *out = malloc(out_cap);
    if (!out) { free(tmpl); return NULL; }

    size_t oi = 0;
    for (size_t i = 0; i < tmpl_len; ) {
        if (tmpl[i] == '{') {
            const char *rep = NULL;
            size_t skip = 0;
            if (strncmp(tmpl + i, "{session_id}", 12) == 0) {
                rep = sid_buf; skip = 12;
            } else if (strncmp(tmpl + i, "{date}", 6) == 0) {
                rep = date_buf; skip = 6;
            } else if (strncmp(tmpl + i, "{agent_name}", 12) == 0) {
                rep = name; skip = 12;
            }
            if (rep) {
                size_t rlen = strlen(rep);
                while (oi + rlen >= out_cap) { out_cap *= 2; out = realloc(out, out_cap); }
                memcpy(out + oi, rep, rlen);
                oi += rlen;
                i += skip;
                continue;
            }
        }
        if (oi + 1 >= out_cap) { out_cap *= 2; out = realloc(out, out_cap); }
        out[oi++] = tmpl[i++];
    }
    out[oi] = '\0';
    free(tmpl);
    return out;
}
