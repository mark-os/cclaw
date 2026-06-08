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
#include <unistd.h>

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

    /* V66: Read access array */
    v = cJSON_GetObjectItemCaseSensitive(root, "read_access");
    if (v && cJSON_IsArray(v)) {
        int cnt = cJSON_GetArraySize(v);
        if (cnt > 0) {
            ac->read_access = malloc((size_t)cnt * sizeof(char *));
            if (ac->read_access) {
                for (int i = 0; i < cnt; i++) {
                    cJSON *item = cJSON_GetArrayItem(v, i);
                    if (cJSON_IsString(item) && item->valuestring)
                        ac->read_access[ac->read_access_count++] = strdup(item->valuestring);
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
    for (size_t i = 0; i < ac->read_access_count; i++) free(ac->read_access[i]);
    free(ac->read_access);
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
    cfg->log_level = global->log_level;
    cfg->save_reasoning = global->save_reasoning;
    cfg->save_usage = global->save_usage;
    cfg->save_logprobs = global->save_logprobs;
    cfg->auto_recall = global->auto_recall;
    cfg->recall_max_tokens = global->recall_max_tokens;

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

/* T150/T196: Create a new agent — dir + workspace + cclaw.db config + system.md */
/* V119/V122/V124: default tool set from agent_config.h */
static const char *AGENT_CREATE_DEFAULT_TOOLS[] = { AGENT_DEFAULT_TOOLS };
static const size_t AGENT_CREATE_DEFAULT_TOOLS_COUNT = AGENT_DEFAULT_TOOLS_COUNT;

/* V124: default config values from agent_config.h */

int agent_config_create(const char *agents_dir, sqlite3 *db, const char *payload_json) {
    if (!agents_dir || !db || !payload_json) return -1;

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

    /* Create workspace subdir */
    char ws[1024];
    snprintf(ws, sizeof(ws), "%s/workspace", dir);
    mkdir(ws, 0755);

    /* V122: clone mode — copy all agent_config rows from source agent */
    cJSON *clone_from = cJSON_GetObjectItemCaseSensitive(payload, "clone_from");
    if (cJSON_IsString(clone_from) && clone_from->valuestring[0]) {
        const char *src = clone_from->valuestring;
        const char *sql = "INSERT OR REPLACE INTO agent_config(agent_name, key, value) "
                          "SELECT ?, key, value FROM agent_config WHERE agent_name=?;";
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, src, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    } else {
        /* Build AgentConfig from payload or defaults */
        AgentConfig ac = {0};
        ac.name = (char *)name;

        cJSON *model = cJSON_GetObjectItemCaseSensitive(payload, "model");
        if (cJSON_IsString(model)) ac.model = model->valuestring;

        cJSON *tools = cJSON_GetObjectItemCaseSensitive(payload, "tools");
        if (cJSON_IsArray(tools)) {
            int cnt = cJSON_GetArraySize(tools);
            char **tool_arr = malloc((size_t)cnt * sizeof(char *));
            if (tool_arr) {
                for (int i = 0; i < cnt; i++) {
                    cJSON *item = cJSON_GetArrayItem(tools, i);
                    if (cJSON_IsString(item)) tool_arr[ac.tool_count++] = item->valuestring;
                }
                ac.tools = tool_arr;
            }
        } else {
            /* V119/V122: seed default tools when not specified */
            ac.tools = (char **)AGENT_CREATE_DEFAULT_TOOLS;
            ac.tool_count = AGENT_CREATE_DEFAULT_TOOLS_COUNT;
        }

        cJSON *hosts = cJSON_GetObjectItemCaseSensitive(payload, "allowed_hosts");
        if (cJSON_IsArray(hosts)) {
            int cnt = cJSON_GetArraySize(hosts);
            char **host_arr = malloc((size_t)cnt * sizeof(char *));
            if (host_arr) {
                for (int i = 0; i < cnt; i++) {
                    cJSON *item = cJSON_GetArrayItem(hosts, i);
                    if (cJSON_IsString(item)) host_arr[ac.allowed_hosts_count++] = item->valuestring;
                }
                ac.allowed_hosts = host_arr;
            }
        }

        /* V122/V124: default max_iterations + shell_timeout */
        ac.max_iterations = AGENT_DEFAULT_MAX_ITERATIONS;

        agent_config_save_db(db, &ac);

        /* Save shell_timeout separately (not in AgentConfig struct) */
        char st_buf[16];
        snprintf(st_buf, sizeof(st_buf), "%d", AGENT_DEFAULT_SHELL_TIMEOUT);
        const char *upsert = "INSERT OR REPLACE INTO agent_config(agent_name, key, value) VALUES(?,?,?);";
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, upsert, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, "shell_timeout", -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, st_buf, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        /* Free only if we allocated (not default array) */
        if (cJSON_IsArray(tools)) free(ac.tools);
        free(ac.allowed_hosts);
    }

    /* Write system.md if provided */
    cJSON *sys_prompt = cJSON_GetObjectItemCaseSensitive(payload, "system_prompt");
    if (cJSON_IsString(sys_prompt) && sys_prompt->valuestring[0]) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/system.md", dir);
        FILE *f = fopen(path, "w");
        if (f) {
            fputs(sys_prompt->valuestring, f);
            fclose(f);
        }
    }

    /* Seed DB agents table row (before freeing payload — name points into it) */
    char name_copy[256];
    snprintf(name_copy, sizeof(name_copy), "%s", name);
    cJSON_Delete(payload);

    AgentRow *row = db_agent_seed(db, agents_dir, name_copy);
    agent_row_free(row);

    return 0;
}

/* Load agent config directly from agents table */
AgentConfig *agent_config_load_db(sqlite3 *db, const char *name) {
    if (!db || !name) return NULL;

    const char *sql = "SELECT model, allowed_tools, allowed_hosts, max_iterations"
                      " FROM agents WHERE name=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);

    AgentConfig *ac = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ac = calloc(1, sizeof(AgentConfig));
        if (!ac) { sqlite3_finalize(stmt); return NULL; }
        ac->name = strdup(name);

        const char *v = (const char *)sqlite3_column_text(stmt, 0);
        if (v) ac->model = strdup(v);

        /* Parse allowed_tools JSON array */
        v = (const char *)sqlite3_column_text(stmt, 1);
        if (v) {
            cJSON *arr = cJSON_Parse(v);
            if (arr && cJSON_IsArray(arr)) {
                int cnt = cJSON_GetArraySize(arr);
                if (cnt > 0) {
                    ac->tools = malloc((size_t)cnt * sizeof(char *));
                    if (ac->tools) {
                        for (int i = 0; i < cnt; i++) {
                            cJSON *item = cJSON_GetArrayItem(arr, i);
                            if (cJSON_IsString(item))
                                ac->tools[ac->tool_count++] = strdup(item->valuestring);
                        }
                    }
                }
            }
            cJSON_Delete(arr);
        }

        /* Parse allowed_hosts JSON array */
        v = (const char *)sqlite3_column_text(stmt, 2);
        if (v) {
            cJSON *arr = cJSON_Parse(v);
            if (arr && cJSON_IsArray(arr)) {
                int cnt = cJSON_GetArraySize(arr);
                if (cnt > 0) {
                    ac->allowed_hosts = malloc((size_t)cnt * sizeof(char *));
                    if (ac->allowed_hosts) {
                        for (int i = 0; i < cnt; i++) {
                            cJSON *item = cJSON_GetArrayItem(arr, i);
                            if (cJSON_IsString(item))
                                ac->allowed_hosts[ac->allowed_hosts_count++] = strdup(item->valuestring);
                        }
                    }
                }
            }
            cJSON_Delete(arr);
        }

        int mi = sqlite3_column_int(stmt, 3);
        if (mi > 0) ac->max_iterations = mi;
    }
    sqlite3_finalize(stmt);
    return ac;
}

/* T196/V80: Save agent config to cclaw.db agent_config table */
int agent_config_save_db(sqlite3 *db, const AgentConfig *ac) {
    (void)db; (void)ac;
    return 0; /* no-op: config now on agents table directly */
}

/* T196: Migrate agent.json → cclaw.db agent_config, delete file after */
int agent_config_migrate_json(sqlite3 *db, const char *agents_dir, const char *name) {
    if (!db || !agents_dir || !name) return -1;

    AgentConfig *ac = agent_config_load(agents_dir, name);
    if (!ac) return 0; /* No file to migrate — not an error */

    int rc = agent_config_save_db(db, ac);
    agent_config_free(ac);
    if (rc != 0) return -1;

    /* Delete agent.json */
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s/agent.json", agents_dir, name);
    unlink(path);
    return 0;
}

/* T144/T196: host management via cclaw.db agent_config table */

int agent_config_add_host(sqlite3 *db, const char *name, const char *host) {
    if (!db || !name || !host || !host[0]) return -1;
    /* Use SQLite json functions on agents.allowed_hosts */
    const char *sql =
        "UPDATE agents SET allowed_hosts = "
        "  CASE WHEN json_array_length(allowed_hosts) = 0 THEN json_array(?2)"
        "  ELSE (SELECT CASE WHEN EXISTS(SELECT 1 FROM json_each(allowed_hosts) WHERE value=?2)"
        "    THEN allowed_hosts ELSE json_insert(allowed_hosts, '$[#]', ?2) END)"
        "  END WHERE name=?1;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, host, -1, SQLITE_STATIC);
    int rc = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

int agent_config_remove_host(sqlite3 *db, const char *name, const char *host) {
    if (!db || !name || !host || !host[0]) return -1;
    const char *sql =
        "UPDATE agents SET allowed_hosts = "
        "  (SELECT json_group_array(value) FROM json_each(agents.allowed_hosts) WHERE value != ?2)"
        " WHERE name=?1;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, host, -1, SQLITE_STATIC);
    int rc = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

char **agent_config_get_hosts(sqlite3 *db, const char *name, size_t *count) {
    *count = 0;
    if (!db || !name) return NULL;
    const char *sql = "SELECT value FROM json_each((SELECT allowed_hosts FROM agents WHERE name=?));";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    size_t cap = 8;
    char **hosts = malloc(cap * sizeof(char *));
    if (!hosts) { sqlite3_finalize(stmt); return NULL; }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(stmt, 0);
        if (!v) continue;
        if (*count >= cap) { cap *= 2; hosts = realloc(hosts, cap * sizeof(char *)); }
        hosts[*count] = strdup(v);
        (*count)++;
    }
    sqlite3_finalize(stmt);
    return hosts;
}

/* T274/V120: Add tool to agent's tools whitelist in cclaw.db agent_config */
int agent_config_add_tool(sqlite3 *db, const char *name, const char *tool) {
    if (!db || !name || !tool || !tool[0]) return -1;
    const char *sql =
        "UPDATE agents SET allowed_tools = "
        "  CASE WHEN json_array_length(allowed_tools) = 0 THEN json_array(?2)"
        "  ELSE (SELECT CASE WHEN EXISTS(SELECT 1 FROM json_each(allowed_tools) WHERE value=?2)"
        "    THEN allowed_tools ELSE json_insert(allowed_tools, '$[#]', ?2) END)"
        "  END WHERE name=?1;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, tool, -1, SQLITE_STATIC);
    int rc = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

/* T186/T196: Create ephemeral agent (V65, V62) — config in cclaw.db */
#include <sys/random.h>
#include <limits.h>

char *agent_create_ephemeral(const char *agents_dir, sqlite3 *db) {
    (void)agents_dir;

    /* Generate UUID v4 */
    uint8_t bytes[16];
    if (getrandom(bytes, 16, 0) != 16) return NULL;
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;

    char uuid[37];
    snprintf(uuid, sizeof(uuid),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0], bytes[1], bytes[2], bytes[3],
        bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11],
        bytes[12], bytes[13], bytes[14], bytes[15]);

    char name[64];
    snprintf(name, sizeof(name), "ephemeral-%s", uuid);

    /* Seed DB row only — no directory creation */
    if (db) {
        db_agent_upsert(db, name, NULL, NULL, NULL);
    }

    return strdup(name);
}

/* T279/V123: Intersect child config with parent ceiling in-place. */
void agent_config_intersect(AgentConfig *child, const AgentConfig *parent) {
    if (!child || !parent) return;

    /* Tools: keep only those present in parent */
    if (parent->tool_count > 0 && child->tool_count > 0) {
        size_t keep = 0;
        for (size_t i = 0; i < child->tool_count; i++) {
            int found = 0;
            for (size_t j = 0; j < parent->tool_count; j++) {
                if (strcmp(child->tools[i], parent->tools[j]) == 0) {
                    found = 1;
                    break;
                }
            }
            if (found) {
                child->tools[keep++] = child->tools[i];
            } else {
                free(child->tools[i]);
            }
        }
        child->tool_count = keep;
    } else if (parent->tool_count > 0 && child->tool_count == 0) {
        /* Child has no explicit tools → adopt parent's set as ceiling */
        child->tools = malloc(parent->tool_count * sizeof(char *));
        if (child->tools) {
            for (size_t i = 0; i < parent->tool_count; i++)
                child->tools[i] = strdup(parent->tools[i]);
            child->tool_count = parent->tool_count;
        }
    }

    /* Hosts: keep only those present in parent */
    if (parent->allowed_hosts_count > 0 && child->allowed_hosts_count > 0) {
        size_t keep = 0;
        for (size_t i = 0; i < child->allowed_hosts_count; i++) {
            int found = 0;
            for (size_t j = 0; j < parent->allowed_hosts_count; j++) {
                if (strcmp(child->allowed_hosts[i], parent->allowed_hosts[j]) == 0) {
                    found = 1;
                    break;
                }
            }
            if (found) {
                child->allowed_hosts[keep++] = child->allowed_hosts[i];
            } else {
                free(child->allowed_hosts[i]);
            }
        }
        child->allowed_hosts_count = keep;
    } else if (parent->allowed_hosts_count > 0 && child->allowed_hosts_count == 0) {
        /* Child has no explicit hosts → adopt parent's */
        child->allowed_hosts = malloc(parent->allowed_hosts_count * sizeof(char *));
        if (child->allowed_hosts) {
            for (size_t i = 0; i < parent->allowed_hosts_count; i++)
                child->allowed_hosts[i] = strdup(parent->allowed_hosts[i]);
            child->allowed_hosts_count = parent->allowed_hosts_count;
        }
    }
    /* If parent has empty hosts → child keeps whatever it has (possibly empty = no network) */

    /* Max iterations: min(child, parent) */
    if (parent->max_iterations > 0) {
        if (child->max_iterations <= 0 || child->max_iterations > parent->max_iterations)
            child->max_iterations = parent->max_iterations;
    }
}
