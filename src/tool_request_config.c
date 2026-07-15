/* request_config tool — handled inline by parent process.
 * Two actions: request_changes (one JSON document batching grants, config
 * values, and a provider definition — one human approval covers it all) and
 * rename_agent. Both park an approval and return NULL; apply_grant (main.c)
 * or admin grant-from-history consumes the parked document via
 * request_config_changes_apply below. */
#define _POSIX_C_SOURCE 200809L
#include "tool_request_config.h"
#include "agent_config.h"
#include "approval.h"
#include "config_registry.h"
#include "db.h"
#include "log.h"
#include "validate.h"
#include "tool_args.h"
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Known provider defaults — eager validation + default fill for the
 * provider section, so a definition that can't apply never parks. */
static const struct {
    const char *name;
    const char *base_url;
    const char *model;
} PROVIDERS[] = {
    {"openrouter", "https://openrouter.ai/api/v1", "deepseek/deepseek-v4-flash"},
    {"gemini",     "https://generativelanguage.googleapis.com/v1beta/openai", "gemini-2.5-flash"},
    {"anthropic",  "https://api.anthropic.com/v1", "claude-sonnet-4-20250514"},
};
#define PROVIDER_COUNT (sizeof(PROVIDERS) / sizeof(PROVIDERS[0]))

static const char *PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"action\":{\"type\":\"string\",\"enum\":[\"request_changes\",\"rename_agent\"],"
    "\"description\":\"Type of config request\"},"
    "\"changes\":{\"type\":\"object\",\"description\":\"For request_changes: any subset of "
    "{grants:{tools:[names],hosts:[hostnames],read_paths:[abs paths],write_paths:[abs paths]}, "
    "agent:{primary_model?,secondary_model?,max_iterations?,shell_timeout?}, "
    "routes:['channel:chat_id',...], "
    "config:{key:value-string,...}, provider:{provider,base_url?,model?,api_key_env?}}. "
    "One human approval covers the whole document. Host prefix '.' covers subdomains "
    "('.example.com' covers example.com AND sub.example.com). agent updates YOUR OWN row "
    "(models accept 'model' or 'model@provider'). routes let you send to a chat via "
    "channel_send (wildcards are operator-only). Config keys must be registered (see "
    "search_config); values are strings. provider: openrouter, gemini, anthropic, or a "
    "custom name with base_url; api_key_env is the secret NAME holding the API key "
    "(defaults to <PROVIDER>_API_KEY) — store the key first with save_secret, never pass "
    "key material. Defining a provider also registers its model, so one document can "
    "define a provider AND point your agent at it via agent.primary_model\"},"
    "\"name\":{\"type\":\"string\",\"description\":\"New agent name (for rename_agent)\"},"
    "\"preamble\":{\"type\":\"string\",\"description\":\"New system prompt preamble (for rename_agent, optional)\"},"
    "\"reason\":{\"type\":\"string\",\"description\":\"Short justification shown to the human approver (optional, recommended)\"}"
    "},\"required\":[\"action\"]}";

/* ── small JSON1 query helpers (all fail-closed: error → non-NULL/-1) ── */

/* First-row/first-column text of sql with one text bind, or NULL. */
static char *q1_text(sqlite3 *db, const char *sql, const char *bind) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(st, 1, bind, -1, SQLITE_STATIC);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) out = strdup(v);
    }
    sqlite3_finalize(st);
    return out;
}

/* Boolean form: 1 iff sql yields a row with non-NULL column 0. */
static int q1_true(sqlite3 *db, const char *sql, const char *bind) {
    char *v = q1_text(db, sql, bind);
    free(v);
    return v != NULL;
}

static char *errf(const char *fmt, const char *a) {
    size_t n = strlen(fmt) + (a ? strlen(a) : 0) + 8;
    char *m = malloc(n);
    if (m) snprintf(m, n, fmt, a ? a : "?");
    return m;
}

/* ── request_changes validation ─────────────────────────────────────────
 * All-or-nothing and typo-hostile: an unknown section or grant kind is an
 * error, never silently dropped. Returns a heap error string, or NULL with
 * *canon_out set to the canonical document (provider defaults filled). */

static const struct { const char *key; const char *path; const char *kind; }
GRANT_KINDS[] = {
    { "tools",       "$.grants.tools",       "tool" },
    { "hosts",       "$.grants.hosts",       "host" },
    { "read_paths",  "$.grants.read_paths",  "read_path" },
    { "write_paths", "$.grants.write_paths", "write_path" },
};
#define GRANT_KIND_COUNT (sizeof(GRANT_KINDS) / sizeof(GRANT_KINDS[0]))

static char *validate_grants(sqlite3 *db, const char *changes) {
    if (!q1_true(db, "SELECT 1 WHERE json_type(?1,'$.grants')='object'", changes))
        return strdup("error: changes.grants must be an object");
    char *bad = q1_text(db,
        "SELECT key FROM json_each(?1,'$.grants')"
        " WHERE key NOT IN ('tools','hosts','read_paths','write_paths') LIMIT 1",
        changes);
    if (bad) {
        char *m = errf("error: unknown grants key '%s' (use tools, hosts, "
                       "read_paths, write_paths)", bad);
        free(bad);
        return m;
    }
    for (size_t i = 0; i < GRANT_KIND_COUNT; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "SELECT 1 WHERE json_type(?1,'%s') IS NOT NULL"
                 " AND json_type(?1,'%s')!='array'",
                 GRANT_KINDS[i].path, GRANT_KINDS[i].path);
        char *t = q1_text(db, sql, changes);
        if (t) {
            free(t);
            return errf("error: grants.%s must be an array of strings",
                        GRANT_KINDS[i].key);
        }
        snprintf(sql, sizeof(sql),
                 "SELECT 1 FROM json_each(?1,'%s')"
                 " WHERE type!='text' OR atom='' LIMIT 1", GRANT_KINDS[i].path);
        t = q1_text(db, sql, changes);
        if (t) {
            free(t);
            return errf("error: grants.%s entries must be non-empty strings",
                        GRANT_KINDS[i].key);
        }
    }
    char *relpath = q1_text(db,
        "SELECT atom FROM ("
        "  SELECT atom FROM json_each(?1,'$.grants.read_paths')"
        "  UNION ALL SELECT atom FROM json_each(?1,'$.grants.write_paths'))"
        " WHERE substr(atom,1,1)!='/' LIMIT 1", changes);
    if (relpath) {
        char *m = errf("error: path grant '%s' must be absolute (start with '/')",
                       relpath);
        free(relpath);
        return m;
    }
    return NULL;
}

static char *validate_config(sqlite3 *db, const char *changes) {
    if (!q1_true(db, "SELECT 1 WHERE json_type(?1,'$.config')='object'", changes))
        return strdup("error: changes.config must be an object of key:value strings");
    char *bad = q1_text(db,
        "SELECT key FROM json_each(?1,'$.config') WHERE type!='text' LIMIT 1",
        changes);
    if (bad) {
        char *m = errf("error: config value for '%s' must be a string", bad);
        free(bad);
        return m;
    }
    /* Every key must be registered (C registry or extension-registered row)
     * and not secret-flagged — config_set would refuse either at apply, so
     * fail now, at request time. */
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "SELECT key FROM json_each(?1,'$.config')",
                           -1, &st, NULL) != SQLITE_OK)
        return strdup("error: config validation failed");
    sqlite3_bind_text(st, 1, changes, -1, SQLITE_STATIC);
    char *err = NULL;
    while (!err && sqlite3_step(st) == SQLITE_ROW) {
        const char *key = (const char *)sqlite3_column_text(st, 0);
        if (!key) continue;
        int known = config_default(key) != NULL, secret = 0;
        sqlite3_stmt *ck;
        if (sqlite3_prepare_v2(db,
                "SELECT default_value IS NOT NULL, COALESCE(secret,0)"
                " FROM config WHERE key=?1", -1, &ck, NULL) == SQLITE_OK) {
            sqlite3_bind_text(ck, 1, key, -1, SQLITE_STATIC);
            if (sqlite3_step(ck) == SQLITE_ROW) {
                known = known || sqlite3_column_int(ck, 0);
                secret = sqlite3_column_int(ck, 1);
            }
            sqlite3_finalize(ck);
        }
        if (!known)
            err = errf("error: unknown config key '%s' — use search_config "
                       "to list registered keys", key);
        else if (secret)
            err = errf("error: config key '%s' is secret — store it with "
                       "save_secret, not set_config", key);
    }
    sqlite3_finalize(st);
    return err;
}

/* Validate $.agent — self-scoped settings on the calling agent's own row.
 * Whitelisted keys only; models may be 'model' or 'model@provider' and must
 * resolve to a models row, OR match the model the same document's provider
 * section defines (one document can define a provider and adopt it).
 * canon_provider is that section's canonical JSON, or NULL. */
static char *validate_agent(sqlite3 *db, const char *changes,
                            const char *canon_provider) {
    if (!q1_true(db, "SELECT 1 WHERE json_type(?1,'$.agent')='object'", changes))
        return strdup("error: changes.agent must be an object");
    char *bad = q1_text(db,
        "SELECT key FROM json_each(?1,'$.agent')"
        " WHERE key NOT IN ('primary_model','secondary_model',"
        "                   'max_iterations','shell_timeout') LIMIT 1",
        changes);
    if (bad) {
        char *m = errf("error: unknown agent key '%s' (use primary_model, "
                       "secondary_model, max_iterations, shell_timeout)", bad);
        free(bad);
        return m;
    }
    bad = q1_text(db,
        "SELECT key FROM json_each(?1,'$.agent')"
        " WHERE key IN ('max_iterations','shell_timeout')"
        " AND (type!='integer' OR CAST(atom AS INTEGER) < 1) LIMIT 1", changes);
    if (bad) {
        char *m = errf("error: agent.%s must be a positive integer", bad);
        free(bad);
        return m;
    }
    /* Model references resolve like llm_proc's candidate lookup: models.id
     * ('model@provider') or bare models.model. */
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT atom FROM json_each(?1,'$.agent')"
            " WHERE key IN ('primary_model','secondary_model')"
            " AND (type!='text' OR atom=''"
            "      OR (NOT EXISTS(SELECT 1 FROM models m"
            "                     WHERE m.id=atom OR m.model=atom)"
            /* NULL-proof: a NULL in a NOT IN list poisons the whole test
             * (unknown → filtered → error suppressed), so absent provider
             * fields collapse to '' which a validated atom can never be. */
            "          AND atom NOT IN ("
            "            COALESCE(json_extract(?2,'$.model'),''),"
            "            COALESCE(json_extract(?2,'$.model'),'')"
            "              ||'@'||COALESCE(json_extract(?2,'$.provider'),''))))"
            " LIMIT 1", -1, &st, NULL) != SQLITE_OK)
        return strdup("error: agent validation failed");
    sqlite3_bind_text(st, 1, changes, -1, SQLITE_STATIC);
    if (canon_provider)
        sqlite3_bind_text(st, 2, canon_provider, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(st, 2);
    char *err = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        err = errf("error: unknown model '%s' — use a registered model "
                   "('model' or 'model@provider'; see search_config), or "
                   "define it in this document's provider section", v);
    }
    sqlite3_finalize(st);
    return err;
}

/* Validate $.routes — 'channel:chat_id' strings. The channel must exist,
 * the chat_id must be literal (wildcard routes are operator-only), and a
 * route already owned by another agent is refused: routes are first-come,
 * like extension names — an agent must not capture another agent's chat. */
static char *validate_routes(sqlite3 *db, const char *changes,
                             const char *agent_name) {
    if (!q1_true(db, "SELECT 1 WHERE json_type(?1,'$.routes')='array'", changes))
        return strdup("error: changes.routes must be an array of "
                      "'channel:chat_id' strings");
    char *bad = q1_text(db,
        "SELECT atom FROM json_each(?1,'$.routes')"
        " WHERE type!='text' OR instr(atom,':') < 2"
        "    OR substr(atom, instr(atom,':')+1) = '' LIMIT 1", changes);
    if (bad) {
        char *m = errf("error: route '%s' must be 'channel:chat_id'", bad);
        free(bad);
        return m;
    }
    bad = q1_text(db,
        "SELECT atom FROM json_each(?1,'$.routes')"
        " WHERE substr(atom, instr(atom,':')+1) = '*' LIMIT 1", changes);
    if (bad) {
        free(bad);
        return strdup("error: wildcard routes are operator-only — request a "
                      "specific chat_id");
    }
    bad = q1_text(db,
        "SELECT atom FROM json_each(?1,'$.routes')"
        " WHERE NOT EXISTS(SELECT 1 FROM channels"
        "                  WHERE name = substr(atom,1,instr(atom,':')-1))"
        " LIMIT 1", changes);
    if (bad) {
        char *m = errf("error: route '%s' names an unknown channel", bad);
        free(bad);
        return m;
    }
    sqlite3_stmt *st;
    char *err = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT atom FROM json_each(?1,'$.routes') j"
            " WHERE EXISTS(SELECT 1 FROM channel_routes c"
            "   WHERE c.channel_name = substr(j.atom,1,instr(j.atom,':')-1)"
            "     AND c.channel_id   = substr(j.atom,instr(j.atom,':')+1)"
            "     AND COALESCE(c.agent_name,'') != ?2) LIMIT 1",
            -1, &st, NULL) != SQLITE_OK)
        return strdup("error: route validation failed");
    sqlite3_bind_text(st, 1, changes, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, agent_name, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        err = errf("error: route '%s' is already owned by another agent", v);
    }
    sqlite3_finalize(st);
    return err;
}

/* Validate $.provider and build its canonical JSON (defaults filled) into
 * *canon_out. Returns a heap error string or NULL. */
static char *validate_provider(sqlite3 *db, const char *changes, char **canon_out) {
    *canon_out = NULL;
    if (!q1_true(db, "SELECT 1 WHERE json_type(?1,'$.provider')='object'", changes))
        return strdup("error: changes.provider must be an object");
    char *bad = q1_text(db,
        "SELECT key FROM json_each(?1,'$.provider')"
        " WHERE key NOT IN ('provider','base_url','model','api_key_env') LIMIT 1",
        changes);
    if (bad) {
        char *m = errf("error: unknown provider key '%s' (use provider, "
                       "base_url, model, api_key_env)", bad);
        free(bad);
        return m;
    }
    char *pj = tool_args_json(db, changes, "provider");
    if (!pj) return strdup("error: changes.provider must be an object");
    char *prov = tool_args_str(db, pj, "provider");
    char *base_url = tool_args_str(db, pj, "base_url");
    char *model = tool_args_str(db, pj, "model");
    char *key_env = tool_args_str(db, pj, "api_key_env");
    free(pj);
    char *err = NULL;
    const char *url_val = NULL, *model_val = NULL, *env_val = NULL;
    char env_buf[96];

    int known = -1;
    if (!prov || !prov[0]) {
        err = strdup("error: provider.provider (the name) is required");
        goto out;
    }
    for (size_t i = 0; i < PROVIDER_COUNT; i++)
        if (strcmp(prov, PROVIDERS[i].name) == 0) { known = (int)i; break; }
    if (known < 0 && (!base_url || !base_url[0])) {
        err = strdup("error: 'base_url' required for unknown providers");
        goto out;
    }
    url_val = (base_url && base_url[0]) ? base_url : PROVIDERS[known].base_url;
    if (strncmp(url_val, "https://", 8) != 0 && strncmp(url_val, "http://", 7) != 0) {
        err = strdup("error: base_url must start with http:// or https://");
        goto out;
    }
    model_val = (model && model[0]) ? model : ((known >= 0) ? PROVIDERS[known].model : NULL);
    /* api_key_env is a secret NAME, never key material. Default derives
     * <PROVIDER>_API_KEY so config_load's env → kv fallback resolves it. */
    if (key_env && key_env[0]) {
        int ok = (key_env[0] >= 'A' && key_env[0] <= 'Z');
        for (const char *c = key_env; ok && *c; c++)
            ok = (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_';
        if (!ok) {
            err = strdup("error: api_key_env must match [A-Z][A-Z0-9_]* — it is "
                         "the secret's NAME, not the key value");
            goto out;
        }
        env_val = key_env;
    } else {
        size_t en = 0;
        for (const char *c = prov; *c && en < sizeof(env_buf) - 12; c++)
            env_buf[en++] = (*c >= 'a' && *c <= 'z') ? (char)(*c - 32)
                          : ((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')) ? *c : '_';
        memcpy(env_buf + en, "_API_KEY", 9);
        env_val = env_buf;
    }
    {
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db,
                "SELECT json_object('provider',?1,'base_url',?2,'model',?3,"
                "'api_key_env',?4)", -1, &st, NULL) != SQLITE_OK) {
            err = strdup("error: failed to build provider JSON");
            goto out;
        }
        sqlite3_bind_text(st, 1, prov, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, url_val, -1, SQLITE_STATIC);
        if (model_val) sqlite3_bind_text(st, 3, model_val, -1, SQLITE_STATIC);
        else sqlite3_bind_null(st, 3);
        sqlite3_bind_text(st, 4, env_val, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(st, 0);
            if (v) *canon_out = strdup(v);
        }
        sqlite3_finalize(st);
        if (!*canon_out) err = strdup("error: failed to build provider JSON");
    }
out:
    free(prov); free(base_url); free(model); free(key_env);
    return err;
}

/* Validate the whole document; on success *canon_out is the canonical
 * changes JSON (grants/config/agent/routes passed through, provider
 * defaults filled). agent_name is the calling agent (route ownership). */
static char *validate_changes(sqlite3 *db, const char *changes,
                              const char *agent_name, char **canon_out) {
    *canon_out = NULL;
    if (!changes || !q1_true(db, "SELECT 1 WHERE json_type(?1)='object'", changes))
        return strdup("error: 'changes' must be a JSON object (see the tool schema)");
    char *bad = q1_text(db,
        "SELECT key FROM json_each(?1)"
        " WHERE key NOT IN ('grants','config','provider','agent','routes')"
        " LIMIT 1", changes);
    if (bad) {
        char *m = errf("error: unknown changes section '%s' (use grants, "
                       "agent, routes, config, provider)", bad);
        free(bad);
        return m;
    }
    char *grants = NULL, *config = NULL, *provider = NULL;
    char *agent = NULL, *routes = NULL, *err = NULL;
    int has_grants = q1_true(db,
        "SELECT 1 WHERE json_type(?1,'$.grants') IS NOT NULL", changes);
    int has_config = q1_true(db,
        "SELECT 1 WHERE json_type(?1,'$.config') IS NOT NULL", changes);
    int has_provider = q1_true(db,
        "SELECT 1 WHERE json_type(?1,'$.provider') IS NOT NULL", changes);
    int has_agent = q1_true(db,
        "SELECT 1 WHERE json_type(?1,'$.agent') IS NOT NULL", changes);
    int has_routes = q1_true(db,
        "SELECT 1 WHERE json_type(?1,'$.routes') IS NOT NULL", changes);

    if (has_grants && (err = validate_grants(db, changes))) goto out;
    if (has_config && (err = validate_config(db, changes))) goto out;
    /* provider before agent: agent.primary_model may adopt the model this
     * same document defines, checked against the canonical provider JSON. */
    if (has_provider && (err = validate_provider(db, changes, &provider))) goto out;
    if (has_agent && (err = validate_agent(db, changes, provider))) goto out;
    if (has_routes && (err = validate_routes(db, changes, agent_name))) goto out;

    /* Reject a document with nothing to apply (all sections absent/empty):
     * parking a no-op approval would only confuse the approver. */
    {
        char *n = q1_text(db,
            "SELECT (SELECT COALESCE(SUM(json_array_length(g.value)),0)"
            "          FROM json_each(?1,'$.grants') g)"
            "     + (SELECT COUNT(*) FROM json_each(?1,'$.config'))"
            "     + (SELECT COUNT(*) FROM json_each(?1,'$.agent'))"
            "     + COALESCE(json_array_length(?1,'$.routes'),0)", changes);
        long total = n ? atol(n) : 0;
        free(n);
        if (total == 0 && !has_provider) {
            err = strdup("error: changes document is empty — nothing to request");
            goto out;
        }
    }

    if (has_grants) grants = tool_args_json(db, changes, "grants");
    if (has_config) config = tool_args_json(db, changes, "config");
    if (has_agent)  agent  = tool_args_json(db, changes, "agent");
    if (has_routes) routes = tool_args_json(db, changes, "routes");

    /* Canonical document: only present sections, minified by json(). */
    {
        char sql[256];
        int off = snprintf(sql, sizeof(sql), "SELECT json_object(");
        int next = 1, first = 1;
        const char *sec[5][2] = {
            {"grants", grants}, {"agent", agent}, {"routes", routes},
            {"config", config}, {"provider", provider}};
        for (int i = 0; i < 5; i++) {
            if (!sec[i][1]) continue;
            off += snprintf(sql + off, sizeof(sql) - (size_t)off, "%s'%s',json(?%d)",
                            first ? "" : ",", sec[i][0], next++);
            first = 0;
        }
        snprintf(sql + off, sizeof(sql) - (size_t)off, ")");
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
            next = 1;
            for (int i = 0; i < 5; i++)
                if (sec[i][1])
                    sqlite3_bind_text(st, next++, sec[i][1], -1, SQLITE_STATIC);
            if (sqlite3_step(st) == SQLITE_ROW) {
                const char *v = (const char *)sqlite3_column_text(st, 0);
                if (v) *canon_out = strdup(v);
            }
            sqlite3_finalize(st);
        }
        if (!*canon_out) err = strdup("error: failed to build changes JSON");
    }
out:
    free(grants); free(config); free(provider); free(agent); free(routes);
    return err;
}

/* ── park paths ───────────────────────────────────────────────────────── */

/* Park a request_changes approval carrying the canonical document. Returns a
 * heap error string on failure, or NULL to signal "parked". */
static char *park_changes(RequestConfigCtx *ctx, const char *canon,
                          const char *reason) {
    /* Dedup: an identical document still pending in this session would queue
     * a second identical prompt (see gate_request's rationale). Both sides
     * are canonically built, so minified-text equality is exact. */
    sqlite3_stmt *chk;
    if (sqlite3_prepare_v2(ctx->db,
            "SELECT 1 FROM approvals WHERE session_id=?1"
            " AND tool_name='request_config' AND action='request_changes'"
            " AND state='pending' AND json_extract(args_json,'$.changes')=?2",
            -1, &chk, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(chk, 1, ctx->session_id);
        sqlite3_bind_text(chk, 2, canon, -1, SQLITE_STATIC);
        if (sqlite3_step(chk) == SQLITE_ROW) {
            sqlite3_finalize(chk);
            return strdup("error: a request for this was already sent and is "
                          "still awaiting the user's yes/no reply — do not "
                          "re-request; wait");
        }
        sqlite3_finalize(chk);
    }

    char *args = NULL;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(ctx->db,
            reason ? "SELECT json_object('action','request_changes',"
                     "'changes',json(?1),'reason',?2)"
                   : "SELECT json_object('action','request_changes',"
                     "'changes',json(?1))",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, canon, -1, SQLITE_STATIC);
        if (reason) sqlite3_bind_text(st, 2, reason, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(st, 0);
            if (v) args = strdup(v);
        }
        sqlite3_finalize(st);
    }
    if (!args) return strdup("error: failed to build args JSON");

    int64_t aid = approval_create(ctx->db, ctx->session_id,
        ctx->current_tool_call_id, "request_config", "request_changes", args,
        "apply");
    free(args);
    if (aid < 0)
        return strdup("error: failed to create approval");

    session_set_state(ctx->db, ctx->session_id, "awaiting_approval");
    return NULL; /* park */
}

/* Park a rename_agent approval. Same dedup contract as park_changes, keyed
 * on the requested name. */
static char *park_rename(RequestConfigCtx *ctx, const char *name,
                         const char *preamble, const char *reason) {
    sqlite3_stmt *chk;
    if (sqlite3_prepare_v2(ctx->db,
            "SELECT 1 FROM approvals WHERE session_id=?1"
            " AND tool_name='request_config' AND action='rename_agent'"
            " AND state='pending' AND json_extract(args_json,'$.name')=?2",
            -1, &chk, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(chk, 1, ctx->session_id);
        sqlite3_bind_text(chk, 2, name, -1, SQLITE_STATIC);
        if (sqlite3_step(chk) == SQLITE_ROW) {
            sqlite3_finalize(chk);
            return strdup("error: a request for this was already sent and is "
                          "still awaiting the user's yes/no reply — do not "
                          "re-request; wait");
        }
        sqlite3_finalize(chk);
    }

    char *args = NULL;
    char sql[192];
    int off = snprintf(sql, sizeof(sql),
                       "SELECT json_object('action','rename_agent','name',?1");
    int next = 2;
    int pre_idx = 0, rsn_idx = 0;
    if (preamble) { pre_idx = next++; off += snprintf(sql + off, sizeof(sql) - (size_t)off, ",'preamble',?%d", pre_idx); }
    if (reason)   { rsn_idx = next++; off += snprintf(sql + off, sizeof(sql) - (size_t)off, ",'reason',?%d", rsn_idx); }
    snprintf(sql + off, sizeof(sql) - (size_t)off, ")");
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(ctx->db, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        if (pre_idx) sqlite3_bind_text(st, pre_idx, preamble, -1, SQLITE_STATIC);
        if (rsn_idx) sqlite3_bind_text(st, rsn_idx, reason, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(st, 0);
            if (v) args = strdup(v);
        }
        sqlite3_finalize(st);
    }
    if (!args) return strdup("error: failed to build args JSON");

    int64_t aid = approval_create(ctx->db, ctx->session_id,
        ctx->current_tool_call_id, "request_config", "rename_agent", args,
        "apply");
    free(args);
    if (aid < 0)
        return strdup("error: failed to create approval");

    session_set_state(ctx->db, ctx->session_id, "awaiting_approval");
    return NULL; /* park */
}

static char *handler(const char *arguments, void *user_data) {
    RequestConfigCtx *ctx = (RequestConfigCtx *)user_data;
    if (!ctx || !ctx->db || !ctx->agent_name)
        return strdup("error: request_config unavailable");

    char *act = tool_args_str(ctx->db, arguments, "action");
    if (!act) return strdup("error: 'action' required (request_changes or rename_agent)");

    /* Optional reason — treat empty string as absent. */
    char *reason = tool_args_str(ctx->db, arguments, "reason");
    if (reason && !reason[0]) { free(reason); reason = NULL; }

    char *result = NULL;

    if (strcmp(act, "request_changes") == 0) {
        char *changes = tool_args_json(ctx->db, arguments, "changes");
        if (!changes) {
            result = strdup("error: 'changes' object required for "
                            "request_changes (see the tool schema)");
        } else {
            char *canon = NULL;
            result = validate_changes(ctx->db, changes, ctx->agent_name, &canon);
            if (!result)
                result = park_changes(ctx, canon, reason);
            free(canon);
        }
        free(changes);

    } else if (strcmp(act, "rename_agent") == 0) {
        char *new_name = tool_args_str(ctx->db, arguments, "name");
        char *preamble = tool_args_str(ctx->db, arguments, "preamble");
        if (!new_name || !new_name[0]) {
            result = strdup("error: 'name' required");
        } else if (!is_valid_agent_name(new_name)) {
            result = strdup("error: agent name must be PascalCase: start with "
                            "an uppercase letter, letters and digits only, max 63 chars");
        } else {
            result = park_rename(ctx, new_name, preamble, reason);
        }
        free(new_name); free(preamble);

    } else {
        result = strdup("error: action must be request_changes or rename_agent");
    }

    free(act); free(reason);
    return result;
}

/* ── apply (shared by main.c apply_grant and admin grant-from-history) ── */

int request_config_changes_apply(sqlite3 *db, const char *agent,
                                 const char *args_json, int64_t expires_at) {
    if (!db || !agent || !args_json) return -1;

    /* Collect every line first, write after — never write inside an open
     * SELECT (read→write upgrade = instant BUSY under WAL). */
    typedef struct { char *kind; char *value; } Line;
    Line *lines = NULL;
    size_t n = 0, cap = 0;
    int rc = 0;

    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT 'tool', atom FROM json_each(?1,'$.changes.grants.tools')"
            " UNION ALL SELECT 'host', atom FROM json_each(?1,'$.changes.grants.hosts')"
            " UNION ALL SELECT 'read_path', atom FROM json_each(?1,'$.changes.grants.read_paths')"
            " UNION ALL SELECT 'write_path', atom FROM json_each(?1,'$.changes.grants.write_paths')"
            " UNION ALL SELECT 'config:'||key, atom FROM json_each(?1,'$.changes.config')",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, args_json, -1, SQLITE_STATIC);
    int step;
    while ((step = sqlite3_step(st)) == SQLITE_ROW) {
        const char *k = (const char *)sqlite3_column_text(st, 0);
        const char *v = (const char *)sqlite3_column_text(st, 1);
        if (!k || !v) { rc = -1; break; }
        if (n >= cap) {
            cap = cap ? cap * 2 : 8;
            Line *t = realloc(lines, cap * sizeof(*lines));
            if (!t) { rc = -1; break; }
            lines = t;
        }
        lines[n].kind = strdup(k);
        lines[n].value = strdup(v);
        if (!lines[n].kind || !lines[n].value) {
            free(lines[n].kind); free(lines[n].value);
            rc = -1; break;
        }
        n++;
    }
    if (step != SQLITE_DONE && rc == 0) rc = -1;
    sqlite3_finalize(st);

    if (rc == 0) {
        sqlite3_exec(db, "SAVEPOINT req_changes", NULL, NULL, NULL);
        for (size_t i = 0; i < n && rc == 0; i++) {
            int lrc;
            if (strncmp(lines[i].kind, "config:", 7) == 0)
                lrc = config_set(db, lines[i].kind + 7, lines[i].value);
            else
                lrc = agent_config_grant(db, agent, lines[i].kind,
                                         lines[i].value, expires_at);
            if (lrc != 0) {
                LOG_WARN_("request_changes apply failed %s=%s",
                          lines[i].kind, lines[i].value);
                rc = -1;
            }
        }
        /* Provider upsert straight from the parked JSON. The API key is NOT
         * here — the document carries only the secret's name (api_key_env);
         * a missing secret is fine (either-order capture: key can land via
         * save_secret or env before or after this grant). */
        if (rc == 0) {
            sqlite3_stmt *ps;
            if (sqlite3_prepare_v2(db,
                    "INSERT INTO providers(name, base_url, endpoint_type, api_key_env, default_model, priority)"
                    " SELECT json_extract(?1,'$.changes.provider.provider'),"
                    "        json_extract(?1,'$.changes.provider.base_url'), 'openai',"
                    "        json_extract(?1,'$.changes.provider.api_key_env'),"
                    "        json_extract(?1,'$.changes.provider.model'),"
                    "        COALESCE((SELECT MAX(priority)+1 FROM providers), 0)"
                    " WHERE json_extract(?1,'$.changes.provider.provider') IS NOT NULL"
                    " ON CONFLICT(name) DO UPDATE SET base_url=excluded.base_url,"
                    "   api_key_env=excluded.api_key_env,"
                    "   default_model=COALESCE(excluded.default_model, default_model)",
                    -1, &ps, NULL) == SQLITE_OK) {
                sqlite3_bind_text(ps, 1, args_json, -1, SQLITE_STATIC);
                if (sqlite3_step(ps) != SQLITE_DONE) rc = -1;
                sqlite3_finalize(ps);
            } else {
                rc = -1;
            }
        }
        /* Seed the provider's default model into `models` (id = model@name,
         * lowest routing priority). Per-request routing joins models →
         * providers, so a provider row without a models row is unreachable
         * except through the empty-table fallback; this makes the parked
         * definition actually routable and lets agent.primary_model adopt
         * 'model@provider' in the same document. */
        if (rc == 0) {
            sqlite3_stmt *ms;
            if (sqlite3_prepare_v2(db,
                    "INSERT OR IGNORE INTO models(id, provider_name, model, priority)"
                    " SELECT json_extract(?1,'$.changes.provider.model')"
                    "          ||'@'||json_extract(?1,'$.changes.provider.provider'),"
                    "        json_extract(?1,'$.changes.provider.provider'),"
                    "        json_extract(?1,'$.changes.provider.model'),"
                    "        (SELECT COALESCE(MAX(priority),0)+1 FROM models)"
                    " WHERE json_extract(?1,'$.changes.provider.model') IS NOT NULL",
                    -1, &ms, NULL) == SQLITE_OK) {
                sqlite3_bind_text(ms, 1, args_json, -1, SQLITE_STATIC);
                if (sqlite3_step(ms) != SQLITE_DONE) rc = -1;
                sqlite3_finalize(ms);
            } else {
                rc = -1;
            }
        }
        /* Self-scoped agent settings — whitelisted columns on the caller's
         * own agents row; absent keys keep their current values. */
        if (rc == 0) {
            sqlite3_stmt *as;
            if (sqlite3_prepare_v2(db,
                    "UPDATE agents SET"
                    " primary_model   = COALESCE(json_extract(?1,'$.changes.agent.primary_model'), primary_model),"
                    " secondary_model = COALESCE(json_extract(?1,'$.changes.agent.secondary_model'), secondary_model),"
                    " max_iterations  = COALESCE(json_extract(?1,'$.changes.agent.max_iterations'), max_iterations),"
                    " shell_timeout   = COALESCE(json_extract(?1,'$.changes.agent.shell_timeout'), shell_timeout)"
                    " WHERE name=?2"
                    " AND json_type(?1,'$.changes.agent')='object'",
                    -1, &as, NULL) == SQLITE_OK) {
                sqlite3_bind_text(as, 1, args_json, -1, SQLITE_STATIC);
                sqlite3_bind_text(as, 2, agent, -1, SQLITE_STATIC);
                if (sqlite3_step(as) != SQLITE_DONE) rc = -1;
                sqlite3_finalize(as);
            } else {
                rc = -1;
            }
        }
        /* Routes: first-come ownership. A route captured by another agent
         * between park and apply fails the whole document (savepoint). The
         * agent asked for send authority, not session mirroring, so the
         * route is 'explicit' — turn output does not auto-deliver there. */
        if (rc == 0) {
            sqlite3_stmt *rs;
            if (sqlite3_prepare_v2(db,
                    "SELECT 1 FROM json_each(?1,'$.changes.routes') j"
                    " WHERE EXISTS(SELECT 1 FROM channel_routes c"
                    "   WHERE c.channel_name = substr(j.atom,1,instr(j.atom,':')-1)"
                    "     AND c.channel_id   = substr(j.atom,instr(j.atom,':')+1)"
                    "     AND COALESCE(c.agent_name,'') != ?2) LIMIT 1",
                    -1, &rs, NULL) == SQLITE_OK) {
                sqlite3_bind_text(rs, 1, args_json, -1, SQLITE_STATIC);
                sqlite3_bind_text(rs, 2, agent, -1, SQLITE_STATIC);
                if (sqlite3_step(rs) == SQLITE_ROW) {
                    LOG_WARN_("request_changes apply: route already owned "
                              "by another agent (agent=%s)", agent);
                    rc = -1;
                }
                sqlite3_finalize(rs);
            } else {
                rc = -1;
            }
        }
        if (rc == 0) {
            sqlite3_stmt *ri;
            if (sqlite3_prepare_v2(db,
                    "INSERT OR IGNORE INTO channel_routes"
                    " (channel_name, channel_id, agent_name, delivery_mode)"
                    " SELECT substr(atom,1,instr(atom,':')-1),"
                    "        substr(atom,instr(atom,':')+1), ?2, 'explicit'"
                    " FROM json_each(?1,'$.changes.routes')",
                    -1, &ri, NULL) == SQLITE_OK) {
                sqlite3_bind_text(ri, 1, args_json, -1, SQLITE_STATIC);
                sqlite3_bind_text(ri, 2, agent, -1, SQLITE_STATIC);
                if (sqlite3_step(ri) != SQLITE_DONE) rc = -1;
                sqlite3_finalize(ri);
            } else {
                rc = -1;
            }
        }
        if (rc == 0)
            sqlite3_exec(db, "RELEASE req_changes", NULL, NULL, NULL);
        else
            sqlite3_exec(db, "ROLLBACK TO req_changes; RELEASE req_changes",
                         NULL, NULL, NULL);
    }

    for (size_t i = 0; i < n; i++) { free(lines[i].kind); free(lines[i].value); }
    free(lines);
    return rc;
}

int tool_request_config_register(ToolRegistry *reg, RequestConfigCtx *ctx) {
    int rc = tools_register(reg, "request_config",
        "Request configuration changes (requires human approval). Action "
        "request_changes takes ONE 'changes' document batching everything you "
        "need — tool grants, host grants (prefix '.' covers subdomains), path "
        "grants (read_paths/write_paths, absolute), your own agent settings "
        "(agent: primary_model/secondary_model/max_iterations/shell_timeout), "
        "channel send routes (routes: ['channel:chat_id']), config values "
        "(registered keys — discover with search_config), and/or an LLM "
        "provider definition (openrouter, gemini, anthropic, or a custom name "
        "with base_url; store the API key first via save_secret and reference "
        "its NAME as api_key_env — never key material). One approval covers "
        "the whole document, so batch related needs into a single request — "
        "e.g. define a provider AND adopt it via agent.primary_model "
        "('model@provider') in one document. Action rename_agent renames this "
        "agent (optional preamble). All actions accept an optional 'reason' "
        "shown to the approver.",
        PARAMS_JSON, handler, ctx);
    if (rc == 0)
        tools_set_recipe(reg, "request_config", (ToolRecipe){EXEC_INLINE, SBX_NONE, NULL});
    return rc;
}
