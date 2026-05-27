#define _POSIX_C_SOURCE 200809L
#include "agent_config.h"
#include "config.h"
#include "db.h"
#include "cJSON.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>

static int str_ends_with(const char *s, const char *suffix) {
    size_t slen = strlen(s);
    size_t suflen = strlen(suffix);
    if (suflen > slen) return 0;
    return strcmp(s + slen - suflen, suffix) == 0;
}

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
    cfg->save_reasoning = global->save_reasoning;
    cfg->save_usage = global->save_usage;
    cfg->save_logprobs = global->save_logprobs;

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

/* T80: scan agents/<name>/skills/ for .md files, concatenate into single string */
char *agent_load_skills(const char *agents_dir, const char *name) {
    if (!agents_dir || !name) return NULL;

    char dir_path[1024];
    int n = snprintf(dir_path, sizeof(dir_path), "%s/%s/skills", agents_dir, name);
    if (n < 0 || (size_t)n >= sizeof(dir_path)) return NULL;

    DIR *d = opendir(dir_path);
    if (!d) return NULL;

    size_t out_cap = 4096;
    size_t out_len = 0;
    char *out = malloc(out_cap);
    if (!out) { closedir(d); return NULL; }
    out[0] = '\0';

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!str_ends_with(ent->d_name, ".md")) continue;

        char fpath[1024];
        int pn = snprintf(fpath, sizeof(fpath), "%s/%s", dir_path, ent->d_name);
        if (pn < 0 || (size_t)pn >= sizeof(fpath)) continue;

        FILE *f = fopen(fpath, "rb");
        if (!f) continue;

        fseek(f, 0, SEEK_END);
        long flen = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (flen <= 0) { fclose(f); continue; }

        /* Grow buffer: existing + newline separator + file content */
        size_t need = out_len + (out_len > 0 ? 1 : 0) + (size_t)flen;
        while (need >= out_cap) { out_cap *= 2; }
        char *tmp = realloc(out, out_cap);
        if (!tmp) { fclose(f); break; }
        out = tmp;

        if (out_len > 0) out[out_len++] = '\n';
        fread(out + out_len, 1, (size_t)flen, f);
        out_len += (size_t)flen;
        out[out_len] = '\0';
        fclose(f);
    }
    closedir(d);

    if (out_len == 0) { free(out); return NULL; }
    return out;
}

/* T122: helper — render template vars in a string */
static char *render_template(const char *tmpl, int64_t session_id,
                             const char *agent_name) {
    if (!tmpl) return NULL;
    size_t tmpl_len = strlen(tmpl);

    char sid_buf[21];
    snprintf(sid_buf, sizeof(sid_buf), "%lld", (long long)session_id);

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char date_buf[11];
    strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm);

    const char *aname = agent_name ? agent_name : "default";

    size_t out_cap = tmpl_len + 128;
    char *out = malloc(out_cap);
    if (!out) return NULL;

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
                rep = aname; skip = 12;
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
    return out;
}

/* T122: Assemble system prompt from DB agent row */
char *agent_build_system_prompt(sqlite3 *db, const char *agent_name,
                                int64_t session_id, const char *agents_dir,
                                const Config *fallback_cfg) {
    /* No agent → fall back to file-based config_render_system_prompt */
    if (!agent_name || !db) {
        return config_render_system_prompt(fallback_cfg, session_id);
    }

    /* Seed from disk if not in DB, then load */
    AgentRow *row = db_agent_seed(db, agents_dir, agent_name);
    if (!row) {
        return config_render_system_prompt(fallback_cfg, session_id);
    }

    /* Render template from DB system_prompt (or fallback to global) */
    const char *tmpl = row->system_prompt;
    char *rendered = NULL;
    if (tmpl && tmpl[0]) {
        rendered = render_template(tmpl, session_id, agent_name);
    } else {
        rendered = config_render_system_prompt(fallback_cfg, session_id);
        agent_row_free(row);
        return rendered;
    }

    /* Load skills from disk (skills stay on filesystem per §D) */
    char *skills = agents_dir ? agent_load_skills(agents_dir, agent_name) : NULL;

    /* T154: Load memory blocks from DB */
    int mb_count = 0;
    MemoryBlock *blocks = memory_block_list(db, agent_name, &mb_count);

    /* Render memory blocks section */
    char *mb_section = NULL;
    size_t mb_len = 0;
    if (blocks && mb_count > 0) {
        /* Estimate size: header + per-block metadata + value */
        size_t est = 64;
        for (int i = 0; i < mb_count; i++) {
            est += 128; /* metadata line overhead */
            est += blocks[i].label ? strlen(blocks[i].label) : 0;
            est += blocks[i].description ? strlen(blocks[i].description) : 0;
            est += blocks[i].value ? strlen(blocks[i].value) : 0;
        }
        mb_section = malloc(est);
        if (mb_section) {
            size_t pos = 0;
            pos += (size_t)snprintf(mb_section + pos, est - pos, "\n\n## Memory Blocks\n");
            for (int i = 0; i < mb_count; i++) {
                int val_len = blocks[i].value ? (int)strlen(blocks[i].value) : 0;
                pos += (size_t)snprintf(mb_section + pos, est - pos,
                    "\n### %s\n"
                    "description: %s\n"
                    "usage: %d/%d chars | %s\n"
                    "---\n%s\n",
                    blocks[i].label ? blocks[i].label : "",
                    blocks[i].description ? blocks[i].description : "",
                    val_len, blocks[i].char_limit,
                    blocks[i].read_only ? "read-only" : "read-write",
                    blocks[i].value ? blocks[i].value : "");
            }
            mb_len = pos;
        }
        memory_block_list_free(blocks, mb_count);
    }

    /* Calculate total size */
    size_t rlen = rendered ? strlen(rendered) : 0;
    size_t skills_len = skills ? strlen(skills) : 0;
    size_t total = rlen + skills_len + mb_len + 128;

    char *out = malloc(total);
    if (!out) {
        free(rendered);
        free(skills);
        free(mb_section);
        agent_row_free(row);
        return NULL;
    }

    size_t oi = 0;
    if (rendered) {
        memcpy(out, rendered, rlen);
        oi = rlen;
    }

    if (mb_len > 0) {
        memcpy(out + oi, mb_section, mb_len); oi += mb_len;
    }

    if (skills_len > 0) {
        const char *hdr = "\n\n## Skills\n";
        size_t hlen = strlen(hdr);
        memcpy(out + oi, hdr, hlen); oi += hlen;
        memcpy(out + oi, skills, skills_len); oi += skills_len;
    }

    out[oi] = '\0';
    free(rendered);
    free(skills);
    free(mb_section);
    agent_row_free(row);
    return out;
}

/* T150: Create a new agent on disk + seed DB */
int agent_config_create(const char *agents_dir, sqlite3 *db, const char *payload_json) {
    if (!agents_dir || !payload_json) return -1;

    cJSON *payload = cJSON_Parse(payload_json);
    if (!payload) return -1;

    cJSON *name_item = cJSON_GetObjectItemCaseSensitive(payload, "name");
    if (!cJSON_IsString(name_item) || !name_item->valuestring[0]) {
        cJSON_Delete(payload);
        return -1;
    }
    const char *name = name_item->valuestring;

    /* Reject names with path separators */
    if (strchr(name, '/') || strchr(name, '\\') || strcmp(name, "..") == 0) {
        cJSON_Delete(payload);
        return -1;
    }

    /* Create agents/<name>/ directory */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/%s", agents_dir, name);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        cJSON_Delete(payload);
        return -1;
    }

    /* Build agent.json from payload fields */
    cJSON *config = cJSON_CreateObject();
    cJSON *model = cJSON_GetObjectItemCaseSensitive(payload, "model");
    if (cJSON_IsString(model))
        cJSON_AddStringToObject(config, "model", model->valuestring);

    cJSON *tools = cJSON_GetObjectItemCaseSensitive(payload, "tools");
    if (cJSON_IsArray(tools))
        cJSON_AddItemToObject(config, "tools", cJSON_Duplicate(tools, 1));

    cJSON *hosts = cJSON_GetObjectItemCaseSensitive(payload, "allowed_hosts");
    if (cJSON_IsArray(hosts))
        cJSON_AddItemToObject(config, "allowed_hosts", cJSON_Duplicate(hosts, 1));

    /* Write agent.json */
    char path[1024];
    snprintf(path, sizeof(path), "%s/agent.json", dir);
    char *json_str = cJSON_Print(config);
    cJSON_Delete(config);
    if (!json_str) { cJSON_Delete(payload); return -1; }

    FILE *f = fopen(path, "w");
    if (!f) { free(json_str); cJSON_Delete(payload); return -1; }
    fputs(json_str, f);
    fclose(f);
    free(json_str);

    /* Write system.md if provided */
    cJSON *sys_prompt = cJSON_GetObjectItemCaseSensitive(payload, "system_prompt");
    if (cJSON_IsString(sys_prompt) && sys_prompt->valuestring[0]) {
        snprintf(path, sizeof(path), "%s/system.md", dir);
        f = fopen(path, "w");
        if (f) {
            fputs(sys_prompt->valuestring, f);
            fclose(f);
        }
    }

    cJSON_Delete(payload);

    /* Seed DB row from newly written files (dir contains agents_dir/name) */
    if (db) {
        const char *seeded_name = strrchr(dir, '/');
        seeded_name = seeded_name ? seeded_name + 1 : dir;
        AgentRow *row = db_agent_seed(db, agents_dir, seeded_name);
        agent_row_free(row);
    }

    return 0;
}

/* T144: helper — read agent.json into cJSON, return root + file path */
static cJSON *agent_json_load(const char *agents_dir, const char *name, char *path_buf, size_t path_cap) {
    if (!agents_dir || !name) return NULL;
    int n = snprintf(path_buf, path_cap, "%s/%s/agent.json", agents_dir, name);
    if (n < 0 || (size_t)n >= path_cap) return NULL;

    FILE *f = fopen(path_buf, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, (size_t)len, f);
    buf[len] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    return root;
}

static int agent_json_save(const char *path, cJSON *root) {
    char *json = cJSON_Print(root);
    if (!json) return -1;
    FILE *f = fopen(path, "w");
    if (!f) { free(json); return -1; }
    fputs(json, f);
    fclose(f);
    free(json);
    return 0;
}

int agent_config_add_host(const char *agents_dir, const char *name, const char *host) {
    if (!host || !host[0]) return -1;
    char path[1024];
    cJSON *root = agent_json_load(agents_dir, name, path, sizeof(path));
    if (!root) return -1;

    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "allowed_hosts");
    if (!arr) {
        arr = cJSON_CreateArray();
        cJSON_AddItemToObject(root, "allowed_hosts", arr);
    }
    /* Check for duplicate */
    cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        if (cJSON_IsString(item) && strcmp(item->valuestring, host) == 0) {
            cJSON_Delete(root);
            return 0; /* already present */
        }
    }
    cJSON_AddItemToArray(arr, cJSON_CreateString(host));
    int rc = agent_json_save(path, root);
    cJSON_Delete(root);
    return rc;
}

int agent_config_remove_host(const char *agents_dir, const char *name, const char *host) {
    if (!host || !host[0]) return -1;
    char path[1024];
    cJSON *root = agent_json_load(agents_dir, name, path, sizeof(path));
    if (!root) return -1;

    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "allowed_hosts");
    if (arr && cJSON_IsArray(arr)) {
        int sz = cJSON_GetArraySize(arr);
        for (int i = 0; i < sz; i++) {
            cJSON *item = cJSON_GetArrayItem(arr, i);
            if (cJSON_IsString(item) && strcmp(item->valuestring, host) == 0) {
                cJSON_DeleteItemFromArray(arr, i);
                break;
            }
        }
    }
    int rc = agent_json_save(path, root);
    cJSON_Delete(root);
    return rc;
}

char **agent_config_get_hosts(const char *agents_dir, const char *name, size_t *count) {
    *count = 0;
    char path[1024];
    cJSON *root = agent_json_load(agents_dir, name, path, sizeof(path));
    if (!root) return NULL;

    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "allowed_hosts");
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(root); return NULL; }

    int sz = cJSON_GetArraySize(arr);
    if (sz <= 0) { cJSON_Delete(root); return NULL; }

    char **hosts = malloc((size_t)sz * sizeof(char *));
    if (!hosts) { cJSON_Delete(root); return NULL; }

    for (int i = 0; i < sz; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        if (cJSON_IsString(item) && item->valuestring)
            hosts[(*count)++] = strdup(item->valuestring);
    }
    cJSON_Delete(root);
    return hosts;
}

/* T186: Create ephemeral agent (V65, V62) */
#include <sys/random.h>
#include <limits.h>

char *agent_create_ephemeral(const char *agents_dir, sqlite3 *db) {
    if (!agents_dir) return NULL;

    /* Generate UUID v4 */
    uint8_t bytes[16];
    if (getrandom(bytes, 16, 0) != 16) return NULL;
    bytes[6] = (bytes[6] & 0x0f) | 0x40; /* version 4 */
    bytes[8] = (bytes[8] & 0x3f) | 0x80; /* variant 1 */

    char uuid[37];
    snprintf(uuid, sizeof(uuid),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0], bytes[1], bytes[2], bytes[3],
        bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11],
        bytes[12], bytes[13], bytes[14], bytes[15]);

    /* Name: ephemeral-<uuid> */
    char name[64];
    snprintf(name, sizeof(name), "ephemeral-%s", uuid);

    /* Create directory structure */
    char buf[PATH_MAX];
    int n = snprintf(buf, sizeof(buf), "%s/%s", agents_dir, name);
    if (n < 0 || (size_t)n >= sizeof(buf)) return NULL;
    if (mkdir(buf, 0755) != 0) return NULL;

    snprintf(buf + n, sizeof(buf) - (size_t)n, "/workspace");
    mkdir(buf, 0755);

    /* Write minimal agent.json (no model = inherit global) */
    buf[n] = '\0';
    snprintf(buf + n, sizeof(buf) - (size_t)n, "/agent.json");
    FILE *f = fopen(buf, "w");
    if (!f) return NULL;
    fputs("{}", f);
    fclose(f);

    /* Create agent.db */
    buf[n] = '\0';
    snprintf(buf + n, sizeof(buf) - (size_t)n, "/agent.db");
    sqlite3 *agent_db = db_open(buf);
    if (agent_db) db_close(agent_db);

    /* Seed into daemon DB if provided */
    if (db) {
        AgentRow *row = db_agent_seed(db, agents_dir, name);
        agent_row_free(row);
    }

    return strdup(name);
}
