#define _DEFAULT_SOURCE        /* realpath() declaration */
#define _POSIX_C_SOURCE 200809L
#include "agent_config.h"
#include "buf.h"
#include "config.h"
#include "config_registry.h"
#include "skills.h"
#include "db.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>

void agent_config_free(AgentConfig *ac) {
    if (!ac) return;
    free(ac->name);
    free(ac->model);
    for (size_t i = 0; i < ac->tool_count; i++) free(ac->tools[i]);
    free(ac->tools);
    free(ac);
}

/* helper — render template vars in a string. {date} stays date-only on
 * purpose: anything finer would bust the provider prompt cache every render
 * (review-1 F22); time-of-day belongs in session_context_text(). */
static char *render_template(const char *tmpl, int64_t session_id,
                             const char *agent_name, const char *agents_dir) {
    if (!tmpl) return NULL;

    char sid_buf[21];
    snprintf(sid_buf, sizeof(sid_buf), "%lld", (long long)session_id);

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char date_buf[11];
    strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm);

    const char *aname = agent_name ? agent_name : "Assistant";

    char ws_buf[PATH_MAX];
    snprintf(ws_buf, sizeof(ws_buf), "%s/%s/workspace",
             agents_dir ? agents_dir : "agents", aname);

    TemplateVar vars[] = {
        {"{session_id}", sid_buf},
        {"{date}", date_buf},
        {"{agent_name}", aname},
        {"{workspace}", ws_buf},
    };
    return template_render(tmpl, vars, 4);
}

/* Assemble system prompt from DB agent row */
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

    /* Render the prompt body: from the DB template when present, else the global
     * fallback (every ephemeral agent has a NULL template). Either way fall
     * through to the memory+skills assembly below so those blocks aren't dropped.
     * A NULL render is a hard failure — don't continue with an empty body. */
    const char *tmpl = row->system_prompt;
    char *rendered = (tmpl && tmpl[0])
        ? render_template(tmpl, session_id, agent_name, agents_dir)
        : config_render_system_prompt(fallback_cfg, session_id);
    if (!rendered) {
        agent_row_free(row);
        return NULL;
    }

    /* Generated tool sections, straight from grants ∩ tools (one query).
     * Deterministic rendering (ORDER BY name) is the cache discipline: the
     * prompt is rebuilt every llm_req, byte-stable until a grant changes —
     * and a mid-session change busting the provider cache once is correct.
     * "Requestable" = registered-but-ungranted; shown ≡ granted holds for
     * the payload, this section is how the agent learns what to ask for. */
    char *tool_section = NULL;
    size_t tool_len = 0;
    {
        const char *tsql =
            "WITH visible AS ("
            "  SELECT t.name, COALESCE(t.description,'') AS description,"
            "    EXISTS (SELECT 1 FROM grants g WHERE g.agent_name=?1"
            "            AND g.kind='tool' AND g.value=t.name"
            "            AND (g.expires_at IS NULL OR g.expires_at > unixepoch()))"
            "      AS granted"
            "  FROM tools t"
            "  WHERE t.enabled=1 AND (t.agent_name IS NULL OR t.agent_name=?1)"
            ")"
            "SELECT COALESCE((SELECT char(10)||char(10)||'## Your Tools'||char(10)||"
            "    group_concat('- '||name||' — '||description, char(10) ORDER BY name)"
            "  FROM visible WHERE granted), '')"
            " || COALESCE((SELECT char(10)||char(10)||'## Requestable Tools'||char(10)||"
            "    'Registered but not granted to you. Ask with request_config'"
            "    ||' (action request_changes, grants.tools) and a reason; an operator approves.'||char(10)||"
            "    group_concat('- '||name||' — '||description, char(10) ORDER BY name)"
            "  FROM visible WHERE NOT granted), '');";
        sqlite3_stmt *ts;
        if (sqlite3_prepare_v2(db, tsql, -1, &ts, NULL) == SQLITE_OK) {
            sqlite3_bind_text(ts, 1, agent_name, -1, SQLITE_STATIC);
            if (sqlite3_step(ts) == SQLITE_ROW) {
                const char *txt = (const char *)sqlite3_column_text(ts, 0);
                if (txt && txt[0]) {
                    tool_section = strdup(txt);
                    tool_len = tool_section ? strlen(tool_section) : 0;
                }
            }
            sqlite3_finalize(ts);
        }
    }

    /* Skill index (progressive disclosure): names+descriptions+locations
     * only — the model file_reads a skill body when a task matches. */
    char *skills = skills_index_build(db, agents_dir, agent_name);

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
            "  FROM memory_blocks b WHERE b.agent_name=?1 AND b.placement='system' ORDER BY b.id"
            ")"
            "SELECT char(10) || char(10) || '## Memory Blocks' || char(10) ||"
            "       group_concat(block_text, '' ORDER BY id)"
            "FROM per_block;";
        if (sqlite3_prepare_v2(db, mb_sql, -1, &mbs, NULL) == SQLITE_OK) {
            sqlite3_bind_text(mbs, 1, agent_name, -1, SQLITE_STATIC);
            if (sqlite3_step(mbs) == SQLITE_ROW && sqlite3_column_type(mbs, 0) != SQLITE_NULL) {
                const char *txt = (const char *)sqlite3_column_text(mbs, 0);
                mb_section = strdup(txt);
                mb_len = mb_section ? strlen(mb_section) : 0;
            }
            sqlite3_finalize(mbs);
        }
    }

    /* Calculate total size */
    size_t rlen = rendered ? strlen(rendered) : 0;
    size_t skills_len = skills ? strlen(skills) : 0;
    size_t total = rlen + tool_len + skills_len + mb_len + 128;

    char *out = malloc(total);
    if (!out) {
        free(rendered);
        free(tool_section);
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

    if (tool_len > 0) {
        memcpy(out + oi, tool_section, tool_len); oi += tool_len;
    }

    if (mb_len > 0) {
        memcpy(out + oi, mb_section, mb_len); oi += mb_len;
    }

    if (skills_len > 0) {
        /* skills_index_build includes its own "## Skills" header */
        memcpy(out + oi, skills, skills_len); oi += skills_len;
    }

    out[oi] = '\0';
    free(rendered);
    free(tool_section);
    free(skills);
    free(mb_section);
    agent_row_free(row);
    return out;
}


/* Load agent config from agents table + grants table */
AgentConfig *agent_config_load_db(sqlite3 *db, const char *name) {
    if (!db || !name) return NULL;

    /* Head of the agent's routing list stands in for the old scalar model. */
    const char *sql =
        "SELECT (SELECT model_id FROM agent_models am"
        "         WHERE am.agent_name=agents.name ORDER BY pos LIMIT 1),"
        "       max_iterations FROM agents WHERE name=?;";
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
                    char **tmp = realloc(ac->tools, cap * sizeof(char *));
                    if (!tmp) break;
                    ac->tools = tmp;
                }
                ac->tools[ac->tool_count++] = strdup(item);
            }
            sqlite3_finalize(js);
        }
    }
    free(tools_json);


    return ac;
}

/* ── Grants API ─────────────────────────────────────────────────── */

/* Is `child` the same path as `parent`, or below it? Both must be canonical
 * (no trailing slash), which realpath() guarantees. */
static int path_is_under(const char *parent, const char *child) {
    size_t n = strlen(parent);
    if (n == 0) return 0;
    if (strncmp(parent, child, n) != 0) return 0;
    return child[n] == '\0' || child[n] == '/' || (n == 1 && parent[0] == '/');
}

int grant_path_hits_db(sqlite3 *db, const char *kind, const char *value) {
    if (!db || !kind || !value || !value[0]) return 0;
    if (strcmp(kind, "read_path") != 0 && strcmp(kind, "write_path") != 0)
        return 0;

    const char *dbf = sqlite3_db_filename(db, "main");
    if (!dbf || !dbf[0]) return 0;             /* :memory: — nothing on disk */
    char db_abs[PATH_MAX];
    if (!realpath(dbf, db_abs)) return 0;      /* not on disk yet */

    /* Canonicalize the grant. A path that does not exist yet cannot be
     * realpath'd, so fall back to its nearest existing ancestor: a grant on a
     * not-yet-created "~/.cclaw/sub" must still be judged against ~/.cclaw. */
    char g_abs[PATH_MAX], tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", value);
    while (!realpath(tmp, g_abs)) {
        char *sl = strrchr(tmp, '/');
        if (!sl) return 0;
        if (sl == tmp) tmp[1] = '\0'; else *sl = '\0';
        if (strcmp(tmp, "/") == 0 && !realpath(tmp, g_abs)) return 0;
    }

    /* The grant covers the DB itself, or a directory holding it. The -wal/-shm
     * siblings live beside it, so a grant naming one of those is caught by the
     * same containment test run the other way round. */
    if (path_is_under(g_abs, db_abs)) return 1;
    char sib[PATH_MAX + 8];
    snprintf(sib, sizeof(sib), "%s-wal", db_abs);
    if (strcmp(g_abs, sib) == 0) return 1;
    snprintf(sib, sizeof(sib), "%s-shm", db_abs);
    if (strcmp(g_abs, sib) == 0) return 1;
    return 0;
}

int agent_config_grant(sqlite3 *db, const char *agent, const char *kind,
                       const char *value, int64_t expires_at) {
    if (!db || !agent || !kind || !value) return -1;
    if (grant_path_hits_db(db, kind, value)) {
        LOG_WARN_("refusing %s grant on cclaw.db path '%s' for agent %s",
                  kind, value, agent);
        return -1;
    }
    /* Canonicalize path grants so stored values are absolute + symlink-resolved
     * (downstream prefix comparisons are only meaningful on canonical paths,
     * and a non-canonical grant could otherwise dodge a naive check). realpath()
     * needs the path to exist; if it doesn't yet, store the literal value —
     * mount-time realpath() canonicalizes when it does. */
    char canon[PATH_MAX];
    if ((strcmp(kind, "read_path") == 0 || strcmp(kind, "write_path") == 0)
        && realpath(value, canon))
        value = canon;
    /* An expired row is authority-dead but still occupies the PK, so a plain
     * INSERT OR IGNORE would make re-granting a once-expired value impossible
     * (observed: prod db_query, 2026-08-10). Revive expired rows — and extend
     * a still-live temporary grant when the operator re-approves it, which is
     * plainly what "grant this again" means. A re-grant never *shortens* a
     * grant's life: the update only fires when it moves expires_at later, and
     * a permanent row (expires_at IS NULL) is left alone. */
    const char *sql =
        "INSERT INTO grants (agent_name, kind, value, expires_at)"
        " VALUES (?1, ?2, ?3, ?4)"
        " ON CONFLICT(agent_name, kind, value) DO UPDATE SET"
        "   expires_at=excluded.expires_at"
        " WHERE grants.expires_at IS NOT NULL"
        "   AND (grants.expires_at <= unixepoch()"
        "        OR excluded.expires_at IS NULL"
        "        OR excluded.expires_at > grants.expires_at);";
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
    /* Upsert: "stop asking" must persist even when no grant row exists yet
     * (e.g. a tool raised to ASK by a policy/hook rather than a standing
     * grant). A bare UPDATE would silently no-op and drop the user's intent. */
    const char *sql =
        "INSERT INTO grants (agent_name, kind, value, approval_mode)"
        " VALUES (?1, 'tool', ?2, ?3)"
        " ON CONFLICT(agent_name, kind, value)"
        " DO UPDATE SET approval_mode=excluded.approval_mode;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, agent, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, tool, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, mode, -1, SQLITE_STATIC);
    int rc = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
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

int grants_contains(sqlite3 *db, const char *agent, const char *kind,
                    const char *value) {
    if (!db || !agent || !kind || !value) return 0;
    const char *sql =
        "SELECT 1 FROM grants WHERE agent_name=?1 AND kind=?2 AND value=?3"
        " AND (expires_at IS NULL OR expires_at > unixepoch());";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, agent, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, kind, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, value, -1, SQLITE_STATIC);
    int found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}

void agent_grant_defaults(sqlite3 *db, const char *agent) {
    if (!db || !agent) return;

    /* agent_default_tools (JSON array) from the config registry — config_get
     * falls back to the registry default when there is no override, so no
     * static fallback list is needed here. Bulk-insert via json_each — same
     * semantics as agent_config_grant with expires_at=0 (NULL in the row). */
    char *list = config_get(db, "agent_default_tools");
    if (!list) return;
    const char *sql =
        "INSERT INTO grants (agent_name, kind, value, expires_at)"
        " SELECT ?1, 'tool', value, NULL FROM json_each(?2)"
        " WHERE true"
        " ON CONFLICT(agent_name, kind, value) DO UPDATE SET"
        "   expires_at=NULL"
        " WHERE grants.expires_at IS NOT NULL"
        "   AND grants.expires_at <= unixepoch();";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, agent, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, list, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    free(list);

    /* Mark approval-required tools */
    char *approval_list = config_get(db, "agent_approval_tools");
    if (approval_list) {
        const char *asql =
            "UPDATE grants SET approval_mode='always'"
            " WHERE agent_name=?1 AND kind='tool' AND value IN (SELECT value FROM json_each(?2));";
        sqlite3_stmt *ast;
        if (sqlite3_prepare_v2(db, asql, -1, &ast, NULL) == SQLITE_OK) {
            sqlite3_bind_text(ast, 1, agent, -1, SQLITE_STATIC);
            sqlite3_bind_text(ast, 2, approval_list, -1, SQLITE_STATIC);
            sqlite3_step(ast);
            sqlite3_finalize(ast);
        }
        free(approval_list);
    }
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
        if (count >= cap) {
            cap *= 2;
            char **tmp = realloc(arr, cap * sizeof(char *));
            if (!tmp) {
                for (size_t i = 0; i < count; i++) free(arr[i]);
                free(arr);
                arr = NULL;
                count = 0;
                break;
            }
            arr = tmp;
        }
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


