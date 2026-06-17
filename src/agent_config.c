#define _POSIX_C_SOURCE 200809L
#include "agent_config.h"
#include "config.h"
#include "db.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
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

void agent_config_free(AgentConfig *ac) {
    if (!ac) return;
    free(ac->name);
    free(ac->model);
    for (size_t i = 0; i < ac->tool_count; i++) free(ac->tools[i]);
    free(ac->tools);
    for (size_t i = 0; i < ac->allowed_hosts_count; i++) free(ac->allowed_hosts[i]);
    free(ac->allowed_hosts);
    for (size_t i = 0; i < ac->read_access_count; i++) free(ac->read_access[i]);
    free(ac->read_access);
    free(ac);
}

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
                while (oi + rlen >= out_cap) {
                    out_cap *= 2;
                    char *tmp = realloc(out, out_cap);
                    if (!tmp) { free(out); free(tmpl); return NULL; }
                    out = tmp;
                }
                memcpy(out + oi, rep, rlen);
                oi += rlen;
                i += skip;
                continue;
            }
        }
        if (oi + 1 >= out_cap) {
            out_cap *= 2;
            char *tmp = realloc(out, out_cap);
            if (!tmp) { free(out); free(tmpl); return NULL; }
            out = tmp;
        }
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
                while (oi + rlen >= out_cap) {
                    out_cap *= 2;
                    char *tmp = realloc(out, out_cap);
                    if (!tmp) { free(out); return NULL; }
                    out = tmp;
                }
                memcpy(out + oi, rep, rlen);
                oi += rlen;
                i += skip;
                continue;
            }
        }
        if (oi + 1 >= out_cap) {
            out_cap *= 2;
            char *tmp = realloc(out, out_cap);
            if (!tmp) { free(out); return NULL; }
            out = tmp;
        }
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
        /* Estimate size: header + per-block metadata + entries */
        size_t est = 64;
        for (int i = 0; i < mb_count; i++) {
            est += 128;
            est += blocks[i].label ? strlen(blocks[i].label) : 0;
            est += blocks[i].description ? strlen(blocks[i].description) : 0;
        }
        /* First pass: gather entries per block to estimate total */
        int *ecounts = calloc((size_t)mb_count, sizeof(int));
        MemoryEntry **elists = calloc((size_t)mb_count, sizeof(MemoryEntry *));
        for (int i = 0; i < mb_count; i++) {
            elists[i] = memory_entries_list(db, agent_name, blocks[i].label, &ecounts[i]);
            for (int j = 0; j < ecounts[i]; j++)
                est += 64 + (elists[i][j].text ? strlen(elists[i][j].text) : 0);
        }
        mb_section = malloc(est);
        if (mb_section) {
            size_t pos = 0;
            pos += (size_t)snprintf(mb_section + pos, est - pos, "\n\n## Memory Blocks\n");
            for (int i = 0; i < mb_count; i++) {
                size_t used = 0;
                for (int j = 0; j < ecounts[i]; j++)
                    used += elists[i][j].text ? strlen(elists[i][j].text) : 0;
                pos += (size_t)snprintf(mb_section + pos, est - pos,
                    "\n### %s\n"
                    "description: %s\n"
                    "usage: %zu/%d chars | %s\n",
                    blocks[i].label ? blocks[i].label : "",
                    blocks[i].description ? blocks[i].description : "",
                    used, blocks[i].char_limit,
                    blocks[i].read_only ? "read-only" : "read-write");
                if (ecounts[i] == 0) {
                    pos += (size_t)snprintf(mb_section + pos, est - pos, "(no entries yet)\n");
                } else {
                    for (int j = 0; j < ecounts[i]; j++)
                        pos += (size_t)snprintf(mb_section + pos, est - pos, "%d. %s\n",
                                                elists[i][j].pos,
                                                elists[i][j].text ? elists[i][j].text : "");
                }
            }
            mb_len = pos;
        }
        for (int i = 0; i < mb_count; i++)
            if (elists[i]) memory_entries_free(elists[i], ecounts[i]);
        free(elists);
        free(ecounts);
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
