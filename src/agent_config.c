#define _DEFAULT_SOURCE        /* realpath() declaration */
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
#include <limits.h>
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

    /* Render memory blocks section — single query (S7/group 5c) */
    char *mb_section = NULL;
    size_t mb_len = 0;
    {
        sqlite3_stmt *mbs = NULL;
        const char *mb_sql =
            "WITH per_block AS ("
            "  SELECT b.id,"
            "    char(10) || '### ' || COALESCE(b.label,'') || char(10) ||"
            "    'description: ' || COALESCE(b.description,'') || char(10) ||"
            "    'usage: ' ||"
            "      (SELECT COALESCE(SUM(length(text)),0) FROM memory_entries e"
            "       WHERE e.agent_name=b.agent_name AND e.block_label=b.label) ||"
            "      '/' || b.char_limit || ' chars | ' ||"
            "      CASE WHEN b.read_only THEN 'read-only' ELSE 'read-write' END || char(10) ||"
            "    COALESCE("
            "      (SELECT group_concat(e.pos || '. ' || e.text, char(10) ORDER BY e.pos) || char(10)"
            "         FROM memory_entries e WHERE e.agent_name=b.agent_name AND e.block_label=b.label),"
            "      '(no entries yet)' || char(10)"
            "    ) AS block_text"
            "  FROM memory_blocks b WHERE b.agent_name=?1 ORDER BY b.id"
            ")"
            "SELECT char(10) || char(10) || '## Memory Blocks' || char(10) ||"
            "       group_concat(block_text, '' ORDER BY id)"
            "FROM per_block;";
        if (sqlite3_prepare_v2(db, mb_sql, -1, &mbs, NULL) == SQLITE_OK) {
            sqlite3_bind_text(mbs, 1, agent_name, -1, SQLITE_STATIC);
            if (sqlite3_step(mbs) == SQLITE_ROW && sqlite3_column_type(mbs, 0) != SQLITE_NULL) {
                const char *txt = (const char *)sqlite3_column_text(mbs, 0);
                mb_section = strdup(txt);
                mb_len = strlen(mb_section);
            }
            sqlite3_finalize(mbs);
        }
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


/* Load agent config from agents table + grants table */
AgentConfig *agent_config_load_db(sqlite3 *db, const char *name) {
    if (!db || !name) return NULL;

    const char *sql = "SELECT model, max_iterations FROM agents WHERE name=?;";
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

        int mi = sqlite3_column_int(stmt, 1);
        if (mi > 0) ac->max_iterations = mi;
    }
    sqlite3_finalize(stmt);
    if (!ac) return NULL;

    /* Load tools from grants */
    char *tools_json = grants_json(db, name, "tool");
    if (tools_json && strcmp(tools_json, "[]") != 0) {
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
    free(tools_json);

    /* Load hosts from grants */
    char *hosts_json = grants_json(db, name, "host");
    if (hosts_json && strcmp(hosts_json, "[]") != 0) {
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
    free(hosts_json);

    return ac;
}

/* ── Grants API ─────────────────────────────────────────────────── */

int agent_config_grant(sqlite3 *db, const char *agent, const char *kind,
                       const char *value, int64_t expires_at) {
    if (!db || !agent || !kind || !value) return -1;
    /* Canonicalize path grants so stored values are absolute + symlink-resolved
     * (downstream prefix comparisons are only meaningful on canonical paths,
     * and a non-canonical grant could otherwise dodge a naive check). realpath()
     * needs the path to exist; if it doesn't yet, store the literal value —
     * mount-time realpath() canonicalizes when it does. */
    char canon[PATH_MAX];
    if ((strcmp(kind, "read_path") == 0 || strcmp(kind, "write_path") == 0)
        && realpath(value, canon))
        value = canon;
    const char *sql =
        "INSERT OR IGNORE INTO grants (agent_name, kind, value, expires_at)"
        " VALUES (?1, ?2, ?3, ?4);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, agent, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, kind, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, value, -1, SQLITE_STATIC);
    if (expires_at > 0)
        sqlite3_bind_int64(stmt, 4, expires_at);
    else
        sqlite3_bind_null(stmt, 4);
    int rc = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

ToolApprovalMode agent_tool_mode(sqlite3 *db, const char *agent, const char *tool) {
    if (!db || !agent || !tool) return TOOL_MODE_SILENT;
    const char *sql =
        "SELECT approval_mode FROM grants"
        " WHERE agent_name=?1 AND kind='tool' AND value=?2"
        " AND (expires_at IS NULL OR expires_at > unixepoch());";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return TOOL_MODE_SILENT;
    sqlite3_bind_text(stmt, 1, agent, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, tool, -1, SQLITE_STATIC);
    ToolApprovalMode mode = TOOL_MODE_SILENT;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *m = (const char *)sqlite3_column_text(stmt, 0);
        if (m && strcmp(m, "always") == 0) mode = TOOL_MODE_ALWAYS;
        else if (m && strcmp(m, "tool_decides") == 0) mode = TOOL_MODE_DECIDES;
    }
    sqlite3_finalize(stmt);
    return mode;
}

int agent_config_set_tool_mode(sqlite3 *db, const char *agent,
                               const char *tool, const char *mode) {
    if (!db || !agent || !tool || !mode) return -1;
    if (strcmp(mode, "silent") != 0 && strcmp(mode, "always") != 0 &&
        strcmp(mode, "tool_decides") != 0)
        return -1;
    const char *sql =
        "UPDATE grants SET approval_mode=?3"
        " WHERE agent_name=?1 AND kind='tool' AND value=?2;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, agent, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, tool, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, mode, -1, SQLITE_STATIC);
    int rc = (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0) ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

int agent_config_revoke(sqlite3 *db, const char *agent, const char *kind,
                        const char *value) {
    if (!db || !agent || !kind || !value) return -1;
    const char *sql = "DELETE FROM grants WHERE agent_name=?1 AND kind=?2 AND value=?3;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, agent, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, kind, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, value, -1, SQLITE_STATIC);
    int rc = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

char *grants_json(sqlite3 *db, const char *agent, const char *kind) {
    if (!db || !agent || !kind) return strdup("[]");
    const char *sql =
        "SELECT json_group_array(value) FROM grants WHERE agent_name=? AND kind=?"
        " AND (expires_at IS NULL OR expires_at > unixepoch());";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return strdup("[]");
    sqlite3_bind_text(stmt, 1, agent, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, kind, -1, SQLITE_STATIC);
    char *result = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(stmt, 0);
        if (v) result = strdup(v);
    }
    sqlite3_finalize(stmt);
    if (!result || strcmp(result, "null") == 0) {
        free(result);
        return strdup("[]");
    }
    return result;
}

/* ── AgentCaps ─────────────────────────────────────────────────── */

static void caps_load_kind(sqlite3 *db, const char *agent, const char *kind,
                           char ***out_arr, size_t *out_count) {
    *out_arr = NULL;
    *out_count = 0;
    char *json = grants_json(db, agent, kind);
    if (!json || strcmp(json, "[]") == 0) { free(json); return; }
    sqlite3_stmt *js;
    if (sqlite3_prepare_v2(db, "SELECT value FROM json_each(?)",
                           -1, &js, NULL) != SQLITE_OK) { free(json); return; }
    sqlite3_bind_text(js, 1, json, -1, SQLITE_STATIC);
    size_t cap = 8;
    char **arr = malloc(cap * sizeof(char *));
    size_t count = 0;
    while (arr && sqlite3_step(js) == SQLITE_ROW) {
        const char *item = (const char *)sqlite3_column_text(js, 0);
        if (!item) continue;
        if (count >= cap) { cap *= 2; arr = realloc(arr, cap * sizeof(char *)); }
        arr[count++] = strdup(item);
    }
    sqlite3_finalize(js);
    free(json);
    *out_arr = arr;
    *out_count = count;
}

void agent_caps_load(sqlite3 *db, const char *agent, AgentCaps *caps) {
    memset(caps, 0, sizeof(*caps));
    if (!db || !agent) return;
    caps_load_kind(db, agent, "host", &caps->hosts, &caps->host_count);
    caps_load_kind(db, agent, "tool", &caps->tools, &caps->tool_count);
    caps_load_kind(db, agent, "read_path", &caps->read_paths, &caps->read_count);
    caps_load_kind(db, agent, "write_path", &caps->write_paths, &caps->write_count);
}

void agent_caps_free(AgentCaps *caps) {
    if (!caps) return;
    for (size_t i = 0; i < caps->host_count; i++) free(caps->hosts[i]);
    free(caps->hosts);
    for (size_t i = 0; i < caps->tool_count; i++) free(caps->tools[i]);
    free(caps->tools);
    for (size_t i = 0; i < caps->read_count; i++) free(caps->read_paths[i]);
    free(caps->read_paths);
    for (size_t i = 0; i < caps->write_count; i++) free(caps->write_paths[i]);
    free(caps->write_paths);
    memset(caps, 0, sizeof(*caps));
}

void agent_caps_refresh(sqlite3 *db, const char *agent, AgentCaps *caps) {
    agent_caps_free(caps);
    agent_caps_load(db, agent, caps);
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
