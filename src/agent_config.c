#define _POSIX_C_SOURCE 200809L
#include "agent_config.h"
#include "config.h"
#include "db.h"
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

    /* Use in-memory SQLite to parse JSON via json_extract/json_each */
    sqlite3 *tmp;
    if (sqlite3_open(":memory:", &tmp) != SQLITE_OK) { free(buf); return NULL; }

    /* Validate JSON */
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(tmp, "SELECT json_valid(?)", -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(tmp); free(buf); return NULL;
    }
    sqlite3_bind_text(stmt, 1, buf, -1, SQLITE_STATIC);
    int valid = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) valid = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    if (!valid) { sqlite3_close(tmp); free(buf); return NULL; }

    AgentConfig *ac = calloc(1, sizeof(AgentConfig));
    if (!ac) { sqlite3_close(tmp); free(buf); return NULL; }
    ac->name = strdup(name);

    /* Extract scalar fields */
    if (sqlite3_prepare_v2(tmp,
        "SELECT json_extract(?1,'$.model'), json_extract(?1,'$.workspace'),"
        " json_extract(?1,'$.max_iterations')", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, buf, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(stmt, 0);
            if (v) ac->model = strdup(v);
            v = (const char *)sqlite3_column_text(stmt, 1);
            if (v) ac->workspace = strdup(v);
            if (sqlite3_column_type(stmt, 2) == SQLITE_INTEGER)
                ac->max_iterations = sqlite3_column_int(stmt, 2);
        }
        sqlite3_finalize(stmt);
    }

    /* Extract array fields via json_each */
    const char *arr_sql = "SELECT value FROM json_each(?1, ?2)";

    /* tools */
    if (sqlite3_prepare_v2(tmp, arr_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, buf, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, "$.tools", -1, SQLITE_STATIC);
        size_t cap = 8;
        ac->tools = malloc(cap * sizeof(char *));
        while (ac->tools && sqlite3_step(stmt) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(stmt, 0);
            if (!v) continue;
            if (ac->tool_count >= cap) { cap *= 2; ac->tools = realloc(ac->tools, cap * sizeof(char *)); }
            ac->tools[ac->tool_count++] = strdup(v);
        }
        sqlite3_finalize(stmt);
        if (ac->tool_count == 0) { free(ac->tools); ac->tools = NULL; }
    }

    /* allowed_hosts */
    if (sqlite3_prepare_v2(tmp, arr_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, buf, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, "$.allowed_hosts", -1, SQLITE_STATIC);
        size_t cap = 8;
        ac->allowed_hosts = malloc(cap * sizeof(char *));
        while (ac->allowed_hosts && sqlite3_step(stmt) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(stmt, 0);
            if (!v) continue;
            if (ac->allowed_hosts_count >= cap) { cap *= 2; ac->allowed_hosts = realloc(ac->allowed_hosts, cap * sizeof(char *)); }
            ac->allowed_hosts[ac->allowed_hosts_count++] = strdup(v);
        }
        sqlite3_finalize(stmt);
        if (ac->allowed_hosts_count == 0) { free(ac->allowed_hosts); ac->allowed_hosts = NULL; }
    }

    /* V66: read_access */
    if (sqlite3_prepare_v2(tmp, arr_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, buf, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, "$.read_access", -1, SQLITE_STATIC);
        size_t cap = 8;
        ac->read_access = malloc(cap * sizeof(char *));
        while (ac->read_access && sqlite3_step(stmt) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(stmt, 0);
            if (!v) continue;
            if (ac->read_access_count >= cap) { cap *= 2; ac->read_access = realloc(ac->read_access, cap * sizeof(char *)); }
            ac->read_access[ac->read_access_count++] = strdup(v);
        }
        sqlite3_finalize(stmt);
        if (ac->read_access_count == 0) { free(ac->read_access); ac->read_access = NULL; }
    }

    sqlite3_close(tmp);
    free(buf);

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

    /* Extract name from payload */
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "SELECT json_extract(?1,'$.name')", -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, payload_json, -1, SQLITE_STATIC);
    const char *name = NULL;
    char name_buf[256] = {0};
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(stmt, 0);
        if (v && v[0]) { snprintf(name_buf, sizeof(name_buf), "%s", v); name = name_buf; }
    }
    sqlite3_finalize(stmt);
    if (!name) return -1;

    /* Reject names with path separators */
    if (strchr(name, '/') || strchr(name, '\\') || strcmp(name, "..") == 0)
        return -1;

    /* Create agents/<name>/ directory */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/%s", agents_dir, name);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST)
        return -1;

    /* Create workspace subdir */
    char ws[1024];
    snprintf(ws, sizeof(ws), "%s/workspace", dir);
    mkdir(ws, 0755);

    /* V122: clone mode */
    const char *clone_check = "SELECT json_extract(?1,'$.clone_from')";
    char *clone_from = NULL;
    if (sqlite3_prepare_v2(db, clone_check, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, payload_json, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(stmt, 0);
            if (v && v[0]) clone_from = strdup(v);
        }
        sqlite3_finalize(stmt);
    }

    if (clone_from) {
        const char *sql = "INSERT OR REPLACE INTO agent_config(agent_name, key, value) "
                          "SELECT ?, key, value FROM agent_config WHERE agent_name=?;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, clone_from, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        free(clone_from);
    } else {
        /* Build AgentConfig from payload or defaults */
        AgentConfig ac = {0};
        ac.name = (char *)name;

        /* Model */
        if (sqlite3_prepare_v2(db, "SELECT json_extract(?1,'$.model')", -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, payload_json, -1, SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *v = (const char *)sqlite3_column_text(stmt, 0);
                if (v) ac.model = (char *)v;
            }
            sqlite3_finalize(stmt);
        }

        /* Tools array */
        char **tool_arr = NULL;
        int has_tools = 0;
        if (sqlite3_prepare_v2(db, "SELECT value FROM json_each(?1,'$.tools')", -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, payload_json, -1, SQLITE_STATIC);
            size_t cap = 16;
            tool_arr = malloc(cap * sizeof(char *));
            while (tool_arr && sqlite3_step(stmt) == SQLITE_ROW) {
                has_tools = 1;
                const char *v = (const char *)sqlite3_column_text(stmt, 0);
                if (!v) continue;
                if (ac.tool_count >= cap) { cap *= 2; tool_arr = realloc(tool_arr, cap * sizeof(char *)); }
                tool_arr[ac.tool_count++] = (char *)v;
            }
            sqlite3_finalize(stmt);
        }
        if (has_tools) {
            ac.tools = tool_arr;
        } else {
            free(tool_arr);
            ac.tools = (char **)AGENT_CREATE_DEFAULT_TOOLS;
            ac.tool_count = AGENT_CREATE_DEFAULT_TOOLS_COUNT;
        }

        /* Allowed hosts */
        char **host_arr = NULL;
        if (sqlite3_prepare_v2(db, "SELECT value FROM json_each(?1,'$.allowed_hosts')", -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, payload_json, -1, SQLITE_STATIC);
            size_t cap = 8;
            host_arr = malloc(cap * sizeof(char *));
            while (host_arr && sqlite3_step(stmt) == SQLITE_ROW) {
                const char *v = (const char *)sqlite3_column_text(stmt, 0);
                if (!v) continue;
                if (ac.allowed_hosts_count >= cap) { cap *= 2; host_arr = realloc(host_arr, cap * sizeof(char *)); }
                host_arr[ac.allowed_hosts_count++] = (char *)v;
            }
            sqlite3_finalize(stmt);
            ac.allowed_hosts = host_arr;
        }

        ac.max_iterations = AGENT_DEFAULT_MAX_ITERATIONS;
        agent_config_save_db(db, &ac);

        /* Save shell_timeout */
        char st_buf[16];
        snprintf(st_buf, sizeof(st_buf), "%d", AGENT_DEFAULT_SHELL_TIMEOUT);
        const char *upsert = "INSERT OR REPLACE INTO agent_config(agent_name, key, value) VALUES(?,?,?);";
        if (sqlite3_prepare_v2(db, upsert, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, "shell_timeout", -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, st_buf, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        if (has_tools) free(ac.tools);
        free(host_arr);
    }

    /* Write system.md if provided */
    if (sqlite3_prepare_v2(db, "SELECT json_extract(?1,'$.system_prompt')", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, payload_json, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *sp = (const char *)sqlite3_column_text(stmt, 0);
            if (sp && sp[0]) {
                char path[1024];
                snprintf(path, sizeof(path), "%s/system.md", dir);
                FILE *f = fopen(path, "w");
                if (f) { fputs(sp, f); fclose(f); }
            }
        }
        sqlite3_finalize(stmt);
    }

    /* Seed DB agents table row */
    AgentRow *row = db_agent_seed(db, agents_dir, name_buf);
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

        /* Parse allowed_tools via json_each() */
        const char *tools_json = (const char *)sqlite3_column_text(stmt, 1);
        if (tools_json && tools_json[0] == '[') {
            sqlite3_stmt *js;
            if (sqlite3_prepare_v2(db, "SELECT value FROM json_each(?)",
                                   -1, &js, NULL) == SQLITE_OK) {
                sqlite3_bind_text(js, 1, tools_json, -1, SQLITE_STATIC);
                size_t cap = 8;
                ac->tools = malloc(cap * sizeof(char *));
                while (ac->tools && sqlite3_step(js) == SQLITE_ROW) {
                    const char *item = (const char *)sqlite3_column_text(js, 0);
                    if (!item) continue;
                    if (ac->tool_count >= cap) {
                        cap *= 2;
                        ac->tools = realloc(ac->tools, cap * sizeof(char *));
                    }
                    ac->tools[ac->tool_count++] = strdup(item);
                }
                sqlite3_finalize(js);
            }
        }

        /* Parse allowed_hosts via json_each() */
        const char *hosts_json = (const char *)sqlite3_column_text(stmt, 2);
        if (hosts_json && hosts_json[0] == '[') {
            sqlite3_stmt *js;
            if (sqlite3_prepare_v2(db, "SELECT value FROM json_each(?)",
                                   -1, &js, NULL) == SQLITE_OK) {
                sqlite3_bind_text(js, 1, hosts_json, -1, SQLITE_STATIC);
                size_t cap = 8;
                ac->allowed_hosts = malloc(cap * sizeof(char *));
                while (ac->allowed_hosts && sqlite3_step(js) == SQLITE_ROW) {
                    const char *item = (const char *)sqlite3_column_text(js, 0);
                    if (!item) continue;
                    if (ac->allowed_hosts_count >= cap) {
                        cap *= 2;
                        ac->allowed_hosts = realloc(ac->allowed_hosts, cap * sizeof(char *));
                    }
                    ac->allowed_hosts[ac->allowed_hosts_count++] = strdup(item);
                }
                sqlite3_finalize(js);
            }
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
