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
#include "config_probe.h"
#include "config_registry.h"
#include "db.h"
#include "log.h"
#include "validate.h"
#include "tool_args.h"
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"action\":{\"type\":\"string\",\"enum\":[\"request_changes\",\"rename_agent\"],"
    "\"description\":\"Type of config request\"},"
    "\"changes\":{\"type\":\"object\",\"description\":\"For request_changes: any subset of "
    "{grants:{tools:[names],hosts:[hostnames],read_paths:[abs paths],write_paths:[abs paths]}, "
    "agent:{primary_model?,secondary_model?,max_iterations?,shell_timeout?}, "
    "routes:['channel:chat_id',...], "
    "config:{key:value-string,...}, provider:{provider,base_url?,api_key_env?}, "
    "models:[{id:'model@provider',context_window?,max_output_tokens?,capabilities?,"
    "priority?,status?}], "
    "secret_bindings:{SECRET_NAME:[hostnames]}}. "
    "One human approval covers the whole document. Host prefix '.' covers subdomains "
    "('.example.com' covers example.com AND sub.example.com). agent updates YOUR OWN row "
    "(models are canonical ids — exactly as listed by search_config). routes let you send "
    "to a chat via "
    "channel_send (wildcards are operator-only). Config keys must be registered (see "
    "search_config); values are strings. provider is TRANSPORT ONLY (endpoint + which "
    "secret pays for it): openrouter, gemini, openai, deepseek, "
    "groq, mistral, cerebras, or a "
    "custom name with base_url; api_key_env is the secret NAME holding the API key "
    "(defaults to <PROVIDER>_API_KEY for a NEW provider) — store the key with save_secret "
    "FIRST, never pass key material. models registers/updates/disables a model on an "
    "already-registered provider (status:'disabled' retires one); one document can "
    "define a provider, register a model on it, AND point your agent at it via "
    "agent.primary_model. "
    "secret_bindings bind a saved secret to the hosts it may be submitted to — a "
    "{{SECRET:X}} call is denied when its target isn't bound; batch every host a "
    "denial named into one document\"},"
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
    /* cclaw.db is never reachable from inside the sandbox — it is the policy
     * store for the containment mechanism itself. Refuse here, at request
     * time, so the agent gets the reason; agent_config_grant refuses again at
     * insert as the structural backstop. */
    sqlite3_stmt *ps;
    if (sqlite3_prepare_v2(db,
            "SELECT 'read_path', atom FROM json_each(?1,'$.grants.read_paths')"
            " UNION ALL"
            " SELECT 'write_path', atom FROM json_each(?1,'$.grants.write_paths')",
            -1, &ps, NULL) != SQLITE_OK)
        return strdup("error: grant validation failed");
    sqlite3_bind_text(ps, 1, changes, -1, SQLITE_STATIC);
    char *err = NULL;
    while (!err && sqlite3_step(ps) == SQLITE_ROW) {
        const char *k = (const char *)sqlite3_column_text(ps, 0);
        const char *v = (const char *)sqlite3_column_text(ps, 1);
        if (k && v && grant_path_hits_db(db, k, v))
            err = errf("error: path grant '%s' covers cclaw.db — the database "
                       "is never reachable from a sandboxed tool, and no grant "
                       "can change that. Use db_query to inspect state.", v);
    }
    sqlite3_finalize(ps);
    return err;
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

/* A binding host is grants.hosts syntax (exact host, or a '.suffix' rule that
 * covers subdomains) with the shape actually checked: hostname characters
 * only, and — since a binding is a durable secret_hosts row a typo would
 * silently never match — no '*' and no bare TLD ('.com' would bind the
 * secret to half the internet). */
static int binding_host_valid(const char *h) {
    if (!h || !h[0]) return 0;
    size_t len = strlen(h);
    if (len > 253) return 0;
    const char *body = h[0] == '.' ? h + 1 : h;
    if (!body[0] || !strchr(body, '.')) return 0;   /* bare TLD / single label */
    for (const char *p = body; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-'))
            return 0;
    }
    return 1;
}

/* Validate $.secret_bindings — {SECRET_NAME: [hosts]}. The secret must
 * already exist agent-scoped (system secrets never interpolate, so a binding
 * for one is meaningless); hosts must be shaped per binding_host_valid;
 * the document is bounded so one park can't smuggle an unreviewable list. */
#define SECRET_BINDINGS_MAX 32

static char *validate_secret_bindings(sqlite3 *db, const char *changes) {
    if (!q1_true(db, "SELECT 1 WHERE json_type(?1,'$.secret_bindings')='object'",
                 changes))
        return strdup("error: changes.secret_bindings must be an object of "
                      "SECRET_NAME: [hostnames]");
    char *bad = q1_text(db,
        "SELECT key FROM json_each(?1,'$.secret_bindings')"
        " WHERE type!='array' LIMIT 1", changes);
    if (bad) {
        char *m = errf("error: secret_bindings.%s must be an array of hostnames", bad);
        free(bad);
        return m;
    }
    bad = q1_text(db,
        "SELECT s.key FROM json_each(?1,'$.secret_bindings') s, json_each(s.value) h"
        " WHERE h.type!='text' OR h.atom='' LIMIT 1", changes);
    if (bad) {
        char *m = errf("error: secret_bindings.%s entries must be non-empty "
                       "hostname strings", bad);
        free(bad);
        return m;
    }
    /* Secret must exist, agent-scoped — a binding request is not the place a
     * secret is born; save_secret first. */
    bad = q1_text(db,
        "SELECT s.key FROM json_each(?1,'$.secret_bindings') s"
        " WHERE NOT EXISTS(SELECT 1 FROM secrets"
        "   WHERE name=s.key AND scope='agent') LIMIT 1", changes);
    if (bad) {
        char *m = errf("error: secret_bindings names unknown secret '%s' — "
                       "store it with save_secret first", bad);
        free(bad);
        return m;
    }
    char *err = NULL;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT h.atom FROM json_each(?1,'$.secret_bindings') s,"
            " json_each(s.value) h", -1, &st, NULL) != SQLITE_OK)
        return strdup("error: secret_bindings validation failed");
    sqlite3_bind_text(st, 1, changes, -1, SQLITE_STATIC);
    int pairs = 0;
    while (!err && sqlite3_step(st) == SQLITE_ROW) {
        const char *h = (const char *)sqlite3_column_text(st, 0);
        if (h && !binding_host_valid(h))
            err = errf("error: secret_bindings host '%s' is not a valid "
                       "hostname or '.suffix' rule", h);
        else if (++pairs > SECRET_BINDINGS_MAX)
            err = strdup("error: too many secret_bindings pairs "
                         "(max 32 per document)");
    }
    sqlite3_finalize(st);
    return err;
}

/* True when this credential name resolves to something: env var or a
 * system-scope secret. Mirrors llm_proc's provider_key_available (existence
 * only, no decrypt) — that is the runtime test this validation front-runs. */
static int key_name_resolves(sqlite3 *db, const char *name) {
    if (!name || !name[0]) return 1;         /* keyless provider — nothing to find */
    const char *v = getenv(name);
    if (v && v[0]) return 1;
    return q1_true(db, "SELECT 1 FROM secrets WHERE name=?1 AND scope='system'",
                   name);
}

/* Suggestion for a model reference that isn't a registered id: the canonical
 * id of a registered model whose bare name matches. NULL when there is none
 * (or when several providers carry the name — then no single "did you mean").
 * Bare names stopped resolving at schema v44; the teaching error is the only
 * thing standing between the agent and a silent mis-route. */
static char *model_id_suggestion(sqlite3 *db, const char *bare) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT id FROM models WHERE model=?1"
            " ORDER BY priority, id LIMIT 1", -1, &st, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(st, 1, bare, -1, SQLITE_STATIC);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) out = strdup(v);
    }
    sqlite3_finalize(st);
    return out;
}

/* "unknown model 'x'" error, with a did-you-mean when a bare name was used
 * for a model that IS registered under a canonical id. */
static char *unknown_model_err(sqlite3 *db, const char *ref) {
    char *sugg = model_id_suggestion(db, ref);
    char buf[512];
    if (sugg)
        snprintf(buf, sizeof(buf),
                 "error: '%s' is a bare model name — models are referenced by "
                 "their canonical id. Did you mean '%s'?", ref, sugg);
    else
        snprintf(buf, sizeof(buf),
                 "error: unknown model '%s' — use a registered model id (see "
                 "search_config), or register it in this document's models "
                 "section ({id:'%s@<provider>'})", ref, ref);
    free(sugg);
    return strdup(buf);
}

/* Validate $.agent — self-scoped settings on the calling agent's own row.
 * Whitelisted keys only; model references must be a canonical models.id (no
 * bare names — see model-id migration, schema v44), either already registered
 * or registered by this same document's models section. canon_models is that
 * section's canonical JSON array, or NULL. */
static char *validate_agent(sqlite3 *db, const char *changes,
                            const char *canon_models) {
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
    /* Canonical-id only: models.id, or an id this document's models section
     * registers. The '[]' fallback keeps json_each valid when the document
     * carries no models section. */
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            /* The outer row must be aliased: an unqualified 'atom' inside the
             * json_each(?2) subquery would bind to that table's own column. */
            "SELECT a.atom FROM json_each(?1,'$.agent') a"
            " WHERE a.key IN ('primary_model','secondary_model')"
            " AND (a.type!='text' OR a.atom=''"
            "      OR (NOT EXISTS(SELECT 1 FROM models m WHERE m.id=a.atom)"
            "          AND NOT EXISTS(SELECT 1 FROM json_each(?2) j"
            "                         WHERE json_extract(j.value,'$.id')=a.atom)))"
            " LIMIT 1", -1, &st, NULL) != SQLITE_OK)
        return strdup("error: agent validation failed");
    sqlite3_bind_text(st, 1, changes, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, canon_models ? canon_models : "[]", -1,
                      SQLITE_STATIC);
    char *err = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        err = v && v[0] ? unknown_model_err(db, v)
                        : strdup("error: agent model must be a non-empty "
                                 "canonical model id");
    }
    sqlite3_finalize(st);
    return err;
}

/* ── $.models — the model catalog surface (2A) ───────────────────────────
 * Parallel to and independent of $.provider: a provider is transport, a model
 * is what actually answers. Registration requires the provider to exist
 * already (or to be defined by this same document) — the inverse of the old
 * "register a model by re-submitting a whole provider doc" trap that knocked
 * prod off its gateway (A1/A7).
 *
 * Canonical form written into *canon_out: every entry gains explicit
 * 'provider' and 'model' fields, so the apply is pure SQL and the approval
 * card can name what is being registered without re-parsing an id. */
static const char *MODEL_KEYS_SQL =
    "('id','context_window','max_output_tokens','capabilities','priority','status')";

/* Split a canonical id at its LAST '@' — model names carry '/' and ':' but a
 * provider name never carries '@'. Returns 0 and fills the halves, or -1. */
static int model_id_split(const char *id, char **model_out, char **prov_out) {
    const char *at = id ? strrchr(id, '@') : NULL;
    if (!at || at == id || !at[1]) return -1;
    size_t mlen = (size_t)(at - id);
    char *m = malloc(mlen + 1);
    char *p = strdup(at + 1);
    if (!m || !p) { free(m); free(p); return -1; }
    memcpy(m, id, mlen);
    m[mlen] = '\0';
    *model_out = m;
    *prov_out = p;
    return 0;
}

/* Shape checks that SQL expresses better than C: one query per rule. */
static char *validate_models_shape(sqlite3 *db, const char *changes) {
    if (!q1_true(db, "SELECT 1 WHERE json_type(?1,'$.models')='array'", changes))
        return strdup("error: changes.models must be an array of model objects "
                      "({id:'model@provider', context_window?, "
                      "max_output_tokens?, capabilities?, priority?, status?})");
    if (q1_true(db, "SELECT 1 FROM json_each(?1,'$.models')"
                    " WHERE type!='object' LIMIT 1", changes))
        return strdup("error: each changes.models entry must be an object");
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT k.key FROM json_each(?1,'$.models') m, json_each(m.value) k"
             " WHERE k.key NOT IN %s LIMIT 1", MODEL_KEYS_SQL);
    char *bad = q1_text(db, sql, changes);
    if (bad) {
        char *e = errf("error: unknown models key '%s' (use id, context_window, "
                       "max_output_tokens, capabilities, priority, status)", bad);
        free(bad);
        return e;
    }
    bad = q1_text(db,
        "SELECT k.key FROM json_each(?1,'$.models') m, json_each(m.value) k"
        " WHERE k.key IN ('context_window','max_output_tokens','priority')"
        " AND (k.type!='integer' OR CAST(k.atom AS INTEGER) < 0) LIMIT 1", changes);
    if (bad) {
        char *e = errf("error: models.%s must be a non-negative integer", bad);
        free(bad);
        return e;
    }
    if (q1_true(db,
            "SELECT 1 FROM json_each(?1,'$.models') m"
            " WHERE json_type(m.value,'$.capabilities') IS NOT NULL"
            "   AND json_type(m.value,'$.capabilities')!='array' LIMIT 1", changes))
        return strdup("error: models.capabilities must be an array of strings "
                      "(e.g. [\"text\",\"image\",\"audio\"])");
    if (q1_true(db,
            "SELECT 1 FROM json_each(?1,'$.models') m,"
            "            json_each(m.value,'$.capabilities') c"
            " WHERE c.type!='text' OR c.atom='' LIMIT 1", changes))
        return strdup("error: models.capabilities entries must be non-empty "
                      "strings");
    bad = q1_text(db,
        "SELECT json_extract(value,'$.status') FROM json_each(?1,'$.models')"
        " WHERE json_extract(value,'$.status') IS NOT NULL"
        "   AND json_extract(value,'$.status') NOT IN ('healthy','disabled')"
        " LIMIT 1", changes);
    if (bad) {
        char *e = errf("error: models.status '%s' must be 'healthy' or "
                       "'disabled'", bad);
        free(bad);
        return e;
    }
    /* Two entries for one id would apply in arbitrary order — the approver
     * could not tell which one wins. */
    if (q1_true(db,
            "SELECT 1 FROM json_each(?1,'$.models')"
            " GROUP BY json_extract(value,'$.id') HAVING COUNT(*) > 1 LIMIT 1",
            changes))
        return strdup("error: changes.models lists the same model id twice");
    return NULL;
}

/* Append one canonicalized entry (provider/model filled in) to *canon. */
static int models_canon_append(sqlite3 *db, char **canon, const char *entry,
                               const char *model, const char *prov) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT json_insert(?1,'$[#]',"
            "  json_patch(json(?2), json_object('model',?3,'provider',?4)))",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, *canon, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, entry, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, model, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, prov, -1, SQLITE_STATIC);
    char *next = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) next = strdup(v);
    }
    sqlite3_finalize(st);
    if (!next) return -1;
    free(*canon);
    *canon = next;
    return 0;
}

/* canon_provider is the $.provider section's canonical JSON (or NULL): a
 * document may define a provider and register models on it at once. */
static char *validate_models(sqlite3 *db, const char *changes,
                             const char *canon_provider, char **canon_out) {
    *canon_out = NULL;
    char *err = validate_models_shape(db, changes);
    if (err) return err;

    char *doc_prov = canon_provider
        ? q1_text(db, "SELECT json_extract(?1,'$.provider')", canon_provider)
        : NULL;
    char *canon = strdup("[]");
    if (!canon) { free(doc_prov); return strdup("error: out of memory"); }

    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT json_extract(value,'$.id'), value"
            " FROM json_each(?1,'$.models')", -1, &st, NULL) != SQLITE_OK) {
        free(canon); free(doc_prov);
        return strdup("error: models validation failed");
    }
    sqlite3_bind_text(st, 1, changes, -1, SQLITE_STATIC);
    int rows = 0;
    while (!err && sqlite3_step(st) == SQLITE_ROW) {
        const char *id = (const char *)sqlite3_column_text(st, 0);
        const char *entry = (const char *)sqlite3_column_text(st, 1);
        rows++;
        if (!id || !id[0] || !entry) {
            err = strdup("error: each changes.models entry needs a non-empty "
                         "'id' (the canonical 'model@provider' id)");
            break;
        }
        /* An id already in the catalog is an update — its provider/model are
         * whatever the row says, seeded ids included (they predate the
         * 'model@provider' convention). */
        char *cur_model = q1_text(db, "SELECT model FROM models WHERE id=?1", id);
        char *cur_prov = NULL;
        if (cur_model)
            cur_prov = q1_text(db, "SELECT provider_name FROM models WHERE id=?1", id);
        if (cur_model && cur_prov) {
            if (models_canon_append(db, &canon, entry, cur_model, cur_prov) != 0)
                err = strdup("error: failed to build models JSON");
            free(cur_model); free(cur_prov);
            continue;
        }
        free(cur_model); free(cur_prov);

        char *nm = NULL, *np = NULL;
        if (model_id_split(id, &nm, &np) != 0) {
            char *sugg = model_id_suggestion(db, id);
            if (sugg) {
                char buf[512];
                snprintf(buf, sizeof(buf),
                         "error: '%s' is a bare model name — models are "
                         "registered and referenced by canonical id. It is "
                         "already registered as '%s'.", id, sugg);
                err = strdup(buf);
                free(sugg);
            } else {
                err = errf("error: model id '%s' must be canonical "
                           "'model@provider' (the provider must already be "
                           "registered — see search_config)", id);
            }
            break;
        }
        /* The inversion: a model is registered ON a provider, never the other
         * way round. */
        int prov_ok = q1_true(db, "SELECT 1 FROM providers WHERE name=?1", np)
                   || (doc_prov && strcmp(doc_prov, np) == 0);
        char *pk = prov_ok ? q1_text(db,
            "SELECT api_key_env FROM providers WHERE name=?1", np) : NULL;
        if (!prov_ok) {
            err = errf("error: provider '%s' is not registered — define it "
                       "first (changes.provider: {provider, base_url, "
                       "api_key_env}), then register models on it", np);
        } else if (pk && !key_name_resolves(db, pk)) {
            /* Registering onto a provider whose key is missing produces a row
             * routing will skip in silence — say so now, not never. */
            err = errf("error: provider's key '%s' does not exist — store it "
                       "with save_secret first, or the model registers into a "
                       "provider routing will skip", pk);
        } else if (models_canon_append(db, &canon, entry, nm, np) != 0) {
            err = strdup("error: failed to build models JSON");
        }
        free(pk); free(nm); free(np);
    }
    sqlite3_finalize(st);
    free(doc_prov);
    if (!err && rows == 0)
        err = strdup("error: changes.models is empty — nothing to register");
    if (err) { free(canon); return err; }
    *canon_out = canon;
    return NULL;
}

/* First route under routes_path already owned by a different agent:
 * 1 (conflict, *atom_out = offending route if requested), 0 (none),
 * -1 (query failed). Shared by eager validation and the park→apply
 * re-check — routes are first-come, an agent must not capture another
 * agent's chat, and a capture between park and apply fails the apply. */
static int route_owned_elsewhere(sqlite3 *db, const char *json,
                                 const char *routes_path, const char *agent,
                                 char **atom_out) {
    if (atom_out) *atom_out = NULL;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT atom FROM json_each(?1,?3) j"
            " WHERE EXISTS(SELECT 1 FROM channel_routes c"
            "   JOIN sessions s ON s.id = c.session_id"
            "   WHERE c.channel_name = substr(j.atom,1,instr(j.atom,':')-1)"
            "     AND c.chat_id      = substr(j.atom,instr(j.atom,':')+1)"
            "     AND s.agent_name != ?2) LIMIT 1",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, agent, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, routes_path, -1, SQLITE_STATIC);
    int hit = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        hit = 1;
        if (atom_out) {
            const char *v = (const char *)sqlite3_column_text(st, 0);
            if (v) *atom_out = strdup(v);
        }
    }
    sqlite3_finalize(st);
    return hit;
}

/* Validate $.routes — 'channel:chat_id' strings. The channel must exist,
 * the chat_id must be literal (wildcard routes are operator-only), and a
 * route already owned by another agent is refused. */
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
        return strdup("error: there are no wildcard routes — a channel-wide "
                      "default is operator config (channels.default_agent); "
                      "request a specific chat_id");
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
    char *atom = NULL;
    int owned = route_owned_elsewhere(db, changes, "$.routes", agent_name, &atom);
    if (owned < 0)
        return strdup("error: route validation failed");
    if (owned) {
        char *m = errf("error: route '%s' is already owned by another agent",
                       atom ? atom : "?");
        free(atom);
        return m;
    }
    return NULL;
}

/* Validate $.provider and build its canonical JSON (defaults filled) into
 * *canon_out. Returns a heap error string or NULL.
 *
 * The provider document is TRANSPORT only — endpoint plus the name of the
 * secret that pays for it. Models are registered through $.models (2A); a
 * 'model' key here used to be the only way to register one, and the
 * masquerade is exactly what took prod's gateway down (A1). */
static char *validate_provider(sqlite3 *db, const char *changes, char **canon_out) {
    *canon_out = NULL;
    if (!q1_true(db, "SELECT 1 WHERE json_type(?1,'$.provider')='object'", changes))
        return strdup("error: changes.provider must be an object");
    if (q1_true(db, "SELECT 1 WHERE json_type(?1,'$.provider.model') IS NOT NULL",
                changes))
        return strdup("error: provider.model is gone — a provider is transport "
                      "(base_url + api_key_env). Register the model in the "
                      "'models' section instead: "
                      "models:[{id:'<model>@<provider>'}]");
    char *bad = q1_text(db,
        "SELECT key FROM json_each(?1,'$.provider')"
        " WHERE key NOT IN ('provider','base_url','api_key_env') LIMIT 1",
        changes);
    if (bad) {
        char *m = errf("error: unknown provider key '%s' (use provider, "
                       "base_url, api_key_env)", bad);
        free(bad);
        return m;
    }
    char *pj = tool_args_json(db, changes, "provider");
    if (!pj) return strdup("error: changes.provider must be an object");
    char *prov = tool_args_str(db, pj, "provider");
    char *base_url = tool_args_str(db, pj, "base_url");
    char *key_env = tool_args_str(db, pj, "api_key_env");
    free(pj);
    char *err = NULL;
    const char *url_val = NULL, *env_val = NULL;
    char env_buf[96];
    char *db_url = NULL, *db_key_env = NULL;
    int row_exists = 0;

    if (!prov || !prov[0]) {
        err = strdup("error: provider.provider (the name) is required");
        goto out;
    }
    /* Known-provider defaults come from the providers table (seeded rows
     * included) — the DB is the one home for provider shape, not a C array. */
    row_exists = q1_true(db, "SELECT 1 FROM providers WHERE name=?1", prov);
    db_url = q1_text(db,
        "SELECT base_url FROM providers WHERE name=?1 AND base_url!=''", prov);
    /* Empty string is a real, deliberate value here (a keyless local gateway),
     * so this read must not filter it out — see A7. */
    db_key_env = q1_text(db, "SELECT api_key_env FROM providers WHERE name=?1",
                         prov);
    if (!db_url && (!base_url || !base_url[0])) {
        err = strdup("error: 'base_url' required for unknown providers");
        goto out;
    }
    url_val = (base_url && base_url[0]) ? base_url : db_url;
    if (strncmp(url_val, "https://", 8) != 0 && strncmp(url_val, "http://", 7) != 0) {
        err = strdup("error: base_url must start with http:// or https://");
        goto out;
    }
    /* api_key_env is a secret NAME, never key material. Default derives
     * <PROVIDER>_API_KEY so config_load's env → kv fallback resolves it. */
    if (key_env && !key_env[0]) {
        /* Explicit "" is the way to say "this endpoint needs no credential" —
         * distinct from absent, which means "leave it as it is". */
        env_val = "";
    } else if (key_env && key_env[0]) {
        int ok = (key_env[0] >= 'A' && key_env[0] <= 'Z');
        for (const char *c = key_env; ok && *c; c++)
            ok = (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_';
        if (!ok) {
            err = strdup("error: api_key_env must match [A-Z][A-Z0-9_]* — it is "
                         "the secret's NAME, not the key value");
            goto out;
        }
        env_val = key_env;
    } else if (row_exists) {
        /* A7: an existing row's credential name is preserved verbatim — the
         * empty string included. A deliberately keyless provider that gets a
         * derived <PROVIDER>_API_KEY back has every one of its models silently
         * dropped from routing at the next request (A8). Absent means "leave
         * it alone", never "guess it again". */
        env_val = db_key_env ? db_key_env : "";
    } else {
        size_t en = 0;
        for (const char *c = prov; *c && en < sizeof(env_buf) - 12; c++)
            env_buf[en++] = (*c >= 'a' && *c <= 'z') ? (char)(*c - 32)
                          : ((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')) ? *c : '_';
        memcpy(env_buf + en, "_API_KEY", 9);
        env_val = env_buf;
    }
    /* Config-time key check (A8's prevention half): at request time the agent
     * can still fix this. At request time the alternative is a routing layer
     * that drops the model with a debug line nobody reads. */
    if (!key_name_resolves(db, env_val)) {
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "error: no key named '%s' exists — neither an environment "
                 "variable nor a saved system secret. Store it first "
                 "(save_secret '%s'), then request the provider; a provider "
                 "whose key cannot be resolved has all its models skipped at "
                 "request time. (A provider that genuinely needs no key: pass "
                 "api_key_env:\"\".)", env_val, env_val);
        err = strdup(buf);
        goto out;
    }
    {
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db,
                "SELECT json_object('provider',?1,'base_url',?2,"
                "'api_key_env',?3)", -1, &st, NULL) != SQLITE_OK) {
            err = strdup("error: failed to build provider JSON");
            goto out;
        }
        sqlite3_bind_text(st, 1, prov, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, url_val, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, env_val, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(st, 0);
            if (v) *canon_out = strdup(v);
        }
        sqlite3_finalize(st);
        if (!*canon_out) err = strdup("error: failed to build provider JSON");
    }
out:
    free(prov); free(base_url); free(key_env);
    free(db_url); free(db_key_env);
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
        " WHERE key NOT IN ('grants','config','provider','models','agent',"
        "                   'routes','secret_bindings')"
        " LIMIT 1", changes);
    if (bad) {
        char *m = errf("error: unknown changes section '%s' (use grants, "
                       "agent, routes, config, provider, models, "
                       "secret_bindings)", bad);
        free(bad);
        return m;
    }
    char *grants = NULL, *config = NULL, *provider = NULL, *models = NULL;
    char *agent = NULL, *routes = NULL, *bindings = NULL, *err = NULL;
    int has_grants = q1_true(db,
        "SELECT 1 WHERE json_type(?1,'$.grants') IS NOT NULL", changes);
    int has_config = q1_true(db,
        "SELECT 1 WHERE json_type(?1,'$.config') IS NOT NULL", changes);
    int has_provider = q1_true(db,
        "SELECT 1 WHERE json_type(?1,'$.provider') IS NOT NULL", changes);
    int has_models = q1_true(db,
        "SELECT 1 WHERE json_type(?1,'$.models') IS NOT NULL", changes);
    int has_agent = q1_true(db,
        "SELECT 1 WHERE json_type(?1,'$.agent') IS NOT NULL", changes);
    int has_routes = q1_true(db,
        "SELECT 1 WHERE json_type(?1,'$.routes') IS NOT NULL", changes);
    int has_bindings = q1_true(db,
        "SELECT 1 WHERE json_type(?1,'$.secret_bindings') IS NOT NULL", changes);

    if (has_grants && (err = validate_grants(db, changes))) goto out;
    if (has_config && (err = validate_config(db, changes))) goto out;
    if (has_bindings && (err = validate_secret_bindings(db, changes))) goto out;
    /* Order is the dependency chain of one document: a provider carries the
     * models registered on it, and the agent may adopt a model this same
     * document registers. */
    if (has_provider && (err = validate_provider(db, changes, &provider))) goto out;
    if (has_models && (err = validate_models(db, changes, provider, &models))) goto out;
    if (has_agent && (err = validate_agent(db, changes, models))) goto out;
    if (has_routes && (err = validate_routes(db, changes, agent_name))) goto out;

    /* Reject a document with nothing to apply (all sections absent/empty):
     * parking a no-op approval would only confuse the approver. */
    {
        char *n = q1_text(db,
            "SELECT (SELECT COALESCE(SUM(json_array_length(g.value)),0)"
            "          FROM json_each(?1,'$.grants') g)"
            "     + (SELECT COUNT(*) FROM json_each(?1,'$.config'))"
            "     + (SELECT COUNT(*) FROM json_each(?1,'$.agent'))"
            "     + (SELECT COALESCE(SUM(json_array_length(s.value)),0)"
            "          FROM json_each(?1,'$.secret_bindings') s)"
            "     + COALESCE(json_array_length(?1,'$.routes'),0)", changes);
        long total = n ? atol(n) : 0;
        free(n);
        if (total == 0 && !has_provider && !has_models) {
            err = strdup("error: changes document is empty — nothing to request");
            goto out;
        }
    }

    if (has_grants)   grants   = tool_args_json(db, changes, "grants");
    if (has_config)   config   = tool_args_json(db, changes, "config");
    if (has_agent)    agent    = tool_args_json(db, changes, "agent");
    if (has_routes)   routes   = tool_args_json(db, changes, "routes");
    if (has_bindings) bindings = tool_args_json(db, changes, "secret_bindings");

    /* Canonical document: only present sections, minified by json(). */
    {
        char sql[384];
        int off = snprintf(sql, sizeof(sql), "SELECT json_object(");
        int next = 1, first = 1;
        const char *sec[7][2] = {
            {"grants", grants}, {"agent", agent}, {"routes", routes},
            {"config", config}, {"provider", provider}, {"models", models},
            {"secret_bindings", bindings}};
        for (int i = 0; i < 7; i++) {
            if (!sec[i][1]) continue;
            off += snprintf(sql + off, sizeof(sql) - (size_t)off, "%s'%s',json(?%d)",
                            first ? "" : ",", sec[i][0], next++);
            first = 0;
        }
        snprintf(sql + off, sizeof(sql) - (size_t)off, ")");
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
            next = 1;
            for (int i = 0; i < 7; i++)
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
    free(grants); free(config); free(provider); free(models); free(agent);
    free(routes); free(bindings);
    return err;
}

/* ── redundancy filter ──────────────────────────────────────────────────
 * Strip grant/route entries the agent already has (exact match only — a
 * subdomain covered by a '.' prefix grant still parks) so the approver only
 * decides what is actually new. Returns the filtered canonical document, or
 * NULL on error; *fully_out = 1 when nothing is left to request at all. */

/* q1_text with a second text bind (agent name). */
static char *q2_text(sqlite3 *db, const char *sql, const char *json,
                     const char *agent) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, agent, -1, SQLITE_STATIC); /* RANGE ok if unused */
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) out = strdup(v);
    }
    sqlite3_finalize(st);
    return out;
}

static char *filter_satisfied(sqlite3 *db, const char *agent,
                              const char *canon, int *fully_out) {
    *fully_out = 0;
    char *cur = strdup(canon);
    if (!cur) return NULL;

    /* Grant arrays: drop values already granted (and not expired). */
    for (size_t i = 0; i < GRANT_KIND_COUNT; i++) {
        char sql[512];
        snprintf(sql, sizeof(sql),
            "SELECT json_set(?1,'%s',json("
            "  (SELECT json_group_array(atom) FROM json_each(?1,'%s') j"
            "   WHERE NOT EXISTS(SELECT 1 FROM grants g"
            "     WHERE g.agent_name=?2 AND g.kind='%s' AND g.value=j.atom"
            "       AND (g.expires_at IS NULL OR g.expires_at>unixepoch())))))"
            " WHERE json_type(?1,'%s')='array'",
            GRANT_KINDS[i].path, GRANT_KINDS[i].path,
            GRANT_KINDS[i].kind, GRANT_KINDS[i].path);
        char *next = q2_text(db, sql, cur, agent);
        if (next) { free(cur); cur = next; }
    }

    /* Routes: drop routes already pinned to one of this agent's sessions
     * (other-agent ownership was already refused by validate_routes). */
    char *next = q2_text(db,
        "SELECT json_set(?1,'$.routes',json("
        "  (SELECT json_group_array(atom) FROM json_each(?1,'$.routes') j"
        "   WHERE NOT EXISTS(SELECT 1 FROM channel_routes c"
        "     JOIN sessions s ON s.id=c.session_id"
        "     WHERE c.channel_name=substr(j.atom,1,instr(j.atom,':')-1)"
        "       AND c.chat_id=substr(j.atom,instr(j.atom,':')+1)"
        "       AND s.agent_name=?2))))"
        " WHERE json_type(?1,'$.routes')='array'", cur, agent);
    if (next) { free(cur); cur = next; }

    /* Secret bindings: drop (secret, host) pairs already recorded (exact
     * match only — a host covered by an existing '.suffix' binding still
     * parks, same contract as the grants filter above). Keys whose hosts all
     * exist drop out of the object entirely. */
    next = q2_text(db,
        "SELECT json_set(?1,'$.secret_bindings',json((SELECT"
        "  COALESCE(json_group_object(k, json(v)),'{}') FROM ("
        "   SELECT s.key AS k,"
        "          (SELECT json_group_array(h.atom) FROM json_each(s.value) h"
        "            WHERE NOT EXISTS(SELECT 1 FROM secret_hosts sh"
        "              WHERE sh.secret_name=s.key AND sh.host=h.atom)) AS v"
        "     FROM json_each(?1,'$.secret_bindings') s"
        "    WHERE (SELECT COUNT(*) FROM json_each(s.value) h"
        "            WHERE NOT EXISTS(SELECT 1 FROM secret_hosts sh"
        "              WHERE sh.secret_name=s.key AND sh.host=h.atom)) > 0))))"
        " WHERE json_type(?1,'$.secret_bindings')='object'", cur, agent);
    if (next) { free(cur); cur = next; }

    /* Drop now-empty arrays, then a now-empty grants object ('$.zz' is a
     * deliberately absent path — json_remove ignores it). */
    next = q2_text(db,
        "SELECT json_remove(?1,"
        " CASE WHEN json_array_length(?1,'$.grants.tools')=0"
        "      THEN '$.grants.tools' ELSE '$.zz' END,"
        " CASE WHEN json_array_length(?1,'$.grants.hosts')=0"
        "      THEN '$.grants.hosts' ELSE '$.zz' END,"
        " CASE WHEN json_array_length(?1,'$.grants.read_paths')=0"
        "      THEN '$.grants.read_paths' ELSE '$.zz' END,"
        " CASE WHEN json_array_length(?1,'$.grants.write_paths')=0"
        "      THEN '$.grants.write_paths' ELSE '$.zz' END,"
        " CASE WHEN json_array_length(?1,'$.routes')=0"
        "      THEN '$.routes' ELSE '$.zz' END)", cur, agent);
    if (next) { free(cur); cur = next; }
    next = q2_text(db,
        "SELECT json_remove(?1,"
        " CASE WHEN json_type(?1,'$.grants')='object'"
        "  AND (SELECT COUNT(*) FROM json_each(?1,'$.grants'))=0"
        " THEN '$.grants' ELSE '$.zz' END,"
        " CASE WHEN json_type(?1,'$.secret_bindings')='object'"
        "  AND (SELECT COUNT(*) FROM json_each(?1,'$.secret_bindings'))=0"
        " THEN '$.secret_bindings' ELSE '$.zz' END)", cur, agent);
    if (next) { free(cur); cur = next; }

    if (q1_true(db, "SELECT 1 WHERE (SELECT COUNT(*) FROM json_each(?1))=0", cur))
        *fully_out = 1;
    return cur;
}

/* ── park paths ───────────────────────────────────────────────────────── */

/* Park a request_changes approval carrying the canonical document. Returns a
 * heap error string on failure, or NULL to signal "parked". */
static char *park_changes(RequestConfigCtx *ctx, const char *canon,
                          const char *reason) {
    /* A denial stands: the same document denied earlier in this session is
     * refused inline instead of re-parked — a "no" is an answer, not a timer.
     * Matched on the canonical changes doc only (reason is commentary for the
     * approver, and a reworded reason must not launder a denied request).
     * auto:expired is excluded — nobody decided, and the expiry notice
     * explicitly invites re-requesting. No time window: a human "no" holds
     * until a human says otherwise or the session ends. */
    sqlite3_stmt *den;
    if (sqlite3_prepare_v2(ctx->db,
            "SELECT 1 FROM approvals WHERE session_id=?1"
            " AND tool_name='request_config' AND action='request_changes'"
            " AND state='denied' AND decided_via != 'auto:expired'"
            " AND json_extract(args_json,'$.changes')=?2",
            -1, &den, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(den, 1, ctx->session_id);
        sqlite3_bind_text(den, 2, canon, -1, SQLITE_STATIC);
        if (sqlite3_step(den) == SQLITE_ROW) {
            sqlite3_finalize(den);
            return strdup("error: this exact change was already denied in "
                          "this session — do not re-request it; adjust your "
                          "approach, or explain to the operator what you were "
                          "trying to do and let them decide");
        }
        sqlite3_finalize(den);
    }

    /* Dedup: an identical document still pending in this session would queue
     * a second identical prompt. Both sides are canonically built, so
     * minified-text equality is exact. */
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
    /* Same denial-stands rule as park_changes, keyed on the requested name. */
    sqlite3_stmt *den;
    if (sqlite3_prepare_v2(ctx->db,
            "SELECT 1 FROM approvals WHERE session_id=?1"
            " AND tool_name='request_config' AND action='rename_agent'"
            " AND state='denied' AND decided_via != 'auto:expired'"
            " AND json_extract(args_json,'$.name')=?2",
            -1, &den, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(den, 1, ctx->session_id);
        sqlite3_bind_text(den, 2, name, -1, SQLITE_STATIC);
        if (sqlite3_step(den) == SQLITE_ROW) {
            sqlite3_finalize(den);
            return strdup("error: this rename was already denied in this "
                          "session — do not re-request it; adjust, or explain "
                          "to the operator and let them decide");
        }
        sqlite3_finalize(den);
    }

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

static char *handler(const char *arguments, void *user_data, int *is_error) {
    RequestConfigCtx *ctx = (RequestConfigCtx *)user_data;
    if (!ctx || !ctx->db || !ctx->agent_name)
        return tool_fail(is_error, "error: request_config unavailable");

    char *act = tool_args_str(ctx->db, arguments, "action");
    if (!act) return tool_fail(is_error, "error: 'action' required (request_changes or rename_agent)");

    /* Optional reason — treat empty string as absent. */
    char *reason = tool_args_str(ctx->db, arguments, "reason");
    if (reason && !reason[0]) { free(reason); reason = NULL; }

    char *result = NULL;

    if (strcmp(act, "request_changes") == 0) {
        char *changes = tool_args_json(ctx->db, arguments, "changes");
        if (!changes) {
            result = tool_fail(is_error, "error: 'changes' object required for "
                            "request_changes (see the tool schema)");
        } else {
            char *canon = NULL;
            /* validate_changes and the park paths return a string ONLY to
             * refuse — one explicit flag where every refusal converges. */
            result = validate_changes(ctx->db, changes, ctx->agent_name, &canon);
            if (result) *is_error = 1;
            if (!result) {
                int fully = 0;
                char *kept = filter_satisfied(ctx->db, ctx->agent_name,
                                              canon, &fully);
                if (!kept)
                    result = tool_fail(is_error, "error: failed to check existing grants");
                else if (fully)
                    result = strdup("already in effect: every grant/route/"
                                    "binding in this request is one you "
                                    "already have — no approval needed, "
                                    "proceed and use it");
                else {
                    result = park_changes(ctx, kept, reason);
                    if (result) *is_error = 1;
                }
                free(kept);
            }
            free(canon);
        }
        free(changes);

    } else if (strcmp(act, "rename_agent") == 0) {
        char *new_name = tool_args_str(ctx->db, arguments, "name");
        char *preamble = tool_args_str(ctx->db, arguments, "preamble");
        if (!new_name || !new_name[0]) {
            result = tool_fail(is_error, "error: 'name' required");
        } else if (!is_valid_agent_name(new_name)) {
            result = tool_fail(is_error, "error: agent name must be PascalCase: start with "
                            "an uppercase letter, letters and digits only, max 63 chars");
        } else {
            result = park_rename(ctx, new_name, preamble, reason);
            if (result) *is_error = 1;
        }
        free(new_name); free(preamble);

    } else {
        result = tool_fail(is_error, "error: action must be request_changes or rename_agent");
    }

    free(act); free(reason);
    return result;
}

/* ── apply (shared by main.c apply_grant and admin grant-from-history) ── */

/* Set *detail_out (if wanted) to a malloc'd copy of msg — first failure wins,
 * so the reported step is the one that actually tripped the rollback. */
static void apply_detail(char **detail_out, const char *fmt, ...) {
    if (!detail_out || *detail_out) return;
    va_list ap;
    va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    *detail_out = strdup(buf);
}

/* Verify-after-write receipt: every line is a RE-READ of the row the section
 * claims to have written, never an echo of the request. Intent and effect
 * diverged in the 2026-08-10 incident ("Gemini is now active" over a config
 * that had silently not taken), and the agent has no other way to tell.
 * ?1 = parked args_json, ?2 = agent name. */
static const char *RECEIPT_SQL =
    "SELECT group_concat(line, '; ') FROM ("
    /* grants — the rows that are live now, by kind and value. */
    "  SELECT 'grants now: '||group_concat(g.kind||' '||g.value, ', ') AS line"
    "    FROM grants g WHERE g.agent_name=?2"
    "     AND (g.expires_at IS NULL OR g.expires_at>unixepoch())"
    "     AND EXISTS(SELECT 1 FROM json_each(?1,'$.changes.grants') k,"
    "                          json_each(k.value) v WHERE v.atom=g.value)"
    "   HAVING line IS NOT NULL"
    /* config — key=value as stored, not as asked. */
    "  UNION ALL"
    "  SELECT 'config now: '||group_concat(c.key||'='||COALESCE(c.value,'(unset)'), ', ')"
    "    FROM config c WHERE c.key IN"
    "     (SELECT key FROM json_each(?1,'$.changes.config'))"
    "   HAVING group_concat(c.key,',') IS NOT NULL"
    "  UNION ALL"
    "  SELECT 'secret bindings now: '||COUNT(*)||' host(s) bound'"
    "    FROM secret_hosts sh WHERE EXISTS("
    "      SELECT 1 FROM json_each(?1,'$.changes.secret_bindings') s,"
    "               json_each(s.value) h"
    "       WHERE s.key=sh.secret_name AND h.atom=sh.host)"
    "   HAVING COUNT(*) > 0"
    /* provider — endpoint and credential NAME as the row now holds them. */
    "  UNION ALL"
    "  SELECT 'provider '||p.name||' -> '||p.base_url||' (key: '"
    "         ||CASE WHEN p.api_key_env='' THEN 'none' ELSE p.api_key_env END||')'"
    "    FROM providers p"
    "   WHERE p.name = json_extract(?1,'$.changes.provider.provider')"
    /* models — one line per requested id, re-read. */
    "  UNION ALL"
    "  SELECT 'model '||m.id||' ('||m.status||', context '"
    "         ||COALESCE(m.context_window,'unset')||')'"
    "    FROM models m WHERE m.id IN"
    "     (SELECT json_extract(value,'$.id') FROM json_each(?1,'$.changes.models'))"
    /* agent — the whole self-scoped row, since any of it may have moved. */
    "  UNION ALL"
    "  SELECT 'your settings now: primary_model='"
    "         ||COALESCE(NULLIF(a.primary_model,''),'(default)')"
    "         ||', secondary_model='"
    "         ||COALESCE(NULLIF(a.secondary_model,''),'(none)')"
    "         ||', max_iterations='||a.max_iterations"
    "         ||', shell_timeout='||a.shell_timeout"
    "    FROM agents a WHERE a.name=?2"
    "     AND json_type(?1,'$.changes.agent')='object'"
    /* routes — bound to one of THIS agent's sessions, verified by join. */
    "  UNION ALL"
    "  SELECT 'routes now bound to you: '||COUNT(*)"
    "    FROM channel_routes c JOIN sessions s ON s.id=c.session_id"
    "   WHERE s.agent_name=?2 AND c.channel_name||':'||c.chat_id IN"
    "     (SELECT atom FROM json_each(?1,'$.changes.routes'))"
    "   HAVING COUNT(*) > 0"
    ")";

int request_config_changes_apply(sqlite3 *db, const char *agent,
                                 const char *args_json, int64_t expires_at,
                                 int64_t session_id, char **detail_out) {
    if (detail_out) *detail_out = NULL;
    if (!db || !agent || !args_json) return -1;

    /* Last-known-good, taken before the first write: if the apply-time probe
     * says the new routing doesn't serve, these are the rows we put back. */
    char *snapshot = config_probe_snapshot(db, agent, args_json);

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
            " UNION ALL SELECT 'config:'||key, atom FROM json_each(?1,'$.changes.config')"
            " UNION ALL SELECT 'secret_bind:'||s.key, h.atom"
            "   FROM json_each(?1,'$.changes.secret_bindings') s, json_each(s.value) h",
            -1, &st, NULL) != SQLITE_OK) {
        free(snapshot);
        return -1;
    }
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
            else if (strncmp(lines[i].kind, "secret_bind:", 12) == 0)
                lrc = db_secret_host_bind(db, lines[i].kind + 12,
                                          lines[i].value);
            else
                lrc = agent_config_grant(db, agent, lines[i].kind,
                                         lines[i].value, expires_at);
            if (lrc != 0) {
                LOG_WARN_("request_changes apply failed %s=%s",
                          lines[i].kind, lines[i].value);
                apply_detail(detail_out, "failed applying %s '%s'",
                             lines[i].kind, lines[i].value);
                rc = -1;
            }
        }
        /* Provider upsert straight from the parked JSON — transport only.
         * The API key is NOT here: the document carries the secret's NAME
         * (api_key_env), validated at request time to actually resolve. */
        if (rc == 0) {
            sqlite3_stmt *ps;
            if (sqlite3_prepare_v2(db,
                    "INSERT INTO providers(name, base_url, endpoint_type, api_key_env, priority)"
                    " SELECT json_extract(?1,'$.changes.provider.provider'),"
                    "        json_extract(?1,'$.changes.provider.base_url'), 'openai',"
                    "        json_extract(?1,'$.changes.provider.api_key_env'),"
                    "        COALESCE((SELECT MAX(priority)+1 FROM providers), 0)"
                    " WHERE json_extract(?1,'$.changes.provider.provider') IS NOT NULL"
                    " ON CONFLICT(name) DO UPDATE SET base_url=excluded.base_url,"
                    "   api_key_env=excluded.api_key_env",
                    -1, &ps, NULL) == SQLITE_OK) {
                sqlite3_bind_text(ps, 1, args_json, -1, SQLITE_STATIC);
                if (sqlite3_step(ps) != SQLITE_DONE) rc = -1;
                sqlite3_finalize(ps);
            } else {
                rc = -1;
            }
            if (rc != 0) apply_detail(detail_out, "failed defining provider");
        }
        /* Models: register missing rows, then update the fields this document
         * actually carries. Two statements rather than one upsert because an
         * absent field must keep the row's current value, and an ON CONFLICT
         * clause cannot tell "absent" from "the default I just computed".
         * providers.default_model is bootstrap sugar now (templates/seed.sql);
         * this is the only path that registers a model at runtime. */
        if (rc == 0 &&
            q1_true(db, "SELECT 1 WHERE json_type(?1,'$.changes.models')='array'",
                    args_json)) {
            static const char *MODEL_SQL[] = {
                "INSERT OR IGNORE INTO models(id, provider_name, model,"
                "  context_window, max_output_tokens, capabilities, priority, status)"
                " SELECT json_extract(value,'$.id'), json_extract(value,'$.provider'),"
                "        json_extract(value,'$.model'),"
                "        json_extract(value,'$.context_window'),"
                "        json_extract(value,'$.max_output_tokens'),"
                "        COALESCE(json_extract(value,'$.capabilities'),'[]'),"
                "        COALESCE(json_extract(value,'$.priority'),"
                "                 (SELECT COALESCE(MAX(priority),0)+1 FROM models)),"
                "        COALESCE(json_extract(value,'$.status'),'healthy')"
                " FROM json_each(?1,'$.changes.models')",
                /* Re-scanned per column so an absent key reads NULL and
                 * COALESCE keeps the stored value. */
                "UPDATE models SET"
                "  context_window = COALESCE((SELECT json_extract(j.value,'$.context_window')"
                "    FROM json_each(?1,'$.changes.models') j"
                "    WHERE json_extract(j.value,'$.id')=models.id), context_window),"
                "  max_output_tokens = COALESCE((SELECT json_extract(j.value,'$.max_output_tokens')"
                "    FROM json_each(?1,'$.changes.models') j"
                "    WHERE json_extract(j.value,'$.id')=models.id), max_output_tokens),"
                "  capabilities = COALESCE((SELECT json_extract(j.value,'$.capabilities')"
                "    FROM json_each(?1,'$.changes.models') j"
                "    WHERE json_extract(j.value,'$.id')=models.id), capabilities),"
                "  priority = COALESCE((SELECT json_extract(j.value,'$.priority')"
                "    FROM json_each(?1,'$.changes.models') j"
                "    WHERE json_extract(j.value,'$.id')=models.id), priority),"
                "  status = COALESCE((SELECT json_extract(j.value,'$.status')"
                "    FROM json_each(?1,'$.changes.models') j"
                "    WHERE json_extract(j.value,'$.id')=models.id), status)"
                " WHERE id IN (SELECT json_extract(value,'$.id')"
                "              FROM json_each(?1,'$.changes.models'))",
            };
            for (size_t i = 0; rc == 0 && i < sizeof(MODEL_SQL) / sizeof(*MODEL_SQL); i++) {
                sqlite3_stmt *ms;
                if (sqlite3_prepare_v2(db, MODEL_SQL[i], -1, &ms, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(ms, 1, args_json, -1, SQLITE_STATIC);
                    if (sqlite3_step(ms) != SQLITE_DONE) rc = -1;
                    sqlite3_finalize(ms);
                } else {
                    rc = -1;
                }
            }
            /* No-op assertion (M3): every requested id must exist now, or the
             * receipt would report a registration that never happened. */
            if (rc == 0 && q1_true(db,
                    "SELECT 1 FROM json_each(?1,'$.changes.models') j"
                    " WHERE NOT EXISTS(SELECT 1 FROM models m"
                    "   WHERE m.id=json_extract(j.value,'$.id')) LIMIT 1",
                    args_json))
                rc = -1;
            if (rc != 0) apply_detail(detail_out, "failed registering models");
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
                /* A 0-row UPDATE means the agents row vanished between park
                 * and apply (rename/delete) — success would be a lie. Only
                 * checked when the document carries an agent section. */
                if (rc == 0 && sqlite3_changes(db) == 0 &&
                    q1_true(db, "SELECT 1 WHERE json_type(?1,'$.changes.agent')"
                                "='object'", args_json)) {
                    apply_detail(detail_out,
                                 "agent row '%s' not found (renamed or "
                                 "deleted since the request parked)", agent);
                    rc = -1;
                }
            } else {
                rc = -1;
            }
            if (rc != 0) apply_detail(detail_out, "failed updating agent settings");
        }
        /* Routes: first-come ownership. A route captured by another agent
         * between park and apply fails the whole document (savepoint). The
         * agent asked for send authority, not session mirroring, so the
         * route is 'explicit' — turn output does not auto-deliver there. */
        if (rc == 0) {
            int owned = route_owned_elsewhere(db, args_json, "$.changes.routes",
                                              agent, NULL);
            if (owned != 0) {
                if (owned > 0) {
                    LOG_WARN_("request_changes apply: route already owned "
                              "by another agent (agent=%s)", agent);
                    apply_detail(detail_out,
                                 "a requested route is already owned by "
                                 "another agent");
                }
                rc = -1;
            }
        }
        if (rc == 0) {
            /* Eager session creation: a route always pins a session, and the
             * session names the agent (route-model unification, v33). Atoms
             * already routed are skipped — same-agent re-requests are no-ops,
             * other-agent captures were refused just above. */
            sqlite3_stmt *ra;
            if (sqlite3_prepare_v2(db,
                    "SELECT substr(atom,1,instr(atom,':')-1),"
                    "       substr(atom,instr(atom,':')+1)"
                    " FROM json_each(?1,'$.changes.routes')"
                    " WHERE NOT EXISTS(SELECT 1 FROM channel_routes c"
                    "   WHERE c.channel_name = substr(atom,1,instr(atom,':')-1)"
                    "     AND c.chat_id      = substr(atom,instr(atom,':')+1))",
                    -1, &ra, NULL) == SQLITE_OK) {
                sqlite3_bind_text(ra, 1, args_json, -1, SQLITE_STATIC);
                while (rc == 0 && sqlite3_step(ra) == SQLITE_ROW) {
                    const char *rch = (const char *)sqlite3_column_text(ra, 0);
                    const char *rchat = (const char *)sqlite3_column_text(ra, 1);
                    sqlite3_stmt *si;
                    if (sqlite3_prepare_v2(db,
                            "INSERT INTO sessions(name, agent_name,"
                            "                     channel_name, chat_id)"
                            " VALUES('route:'||?1||':'||?2, ?3, ?1, ?2)",
                            -1, &si, NULL) != SQLITE_OK) { rc = -1; break; }
                    sqlite3_bind_text(si, 1, rch, -1, SQLITE_STATIC);
                    sqlite3_bind_text(si, 2, rchat, -1, SQLITE_STATIC);
                    sqlite3_bind_text(si, 3, agent, -1, SQLITE_STATIC);
                    if (sqlite3_step(si) != SQLITE_DONE) rc = -1;
                    sqlite3_finalize(si);
                    if (rc != 0) break;
                    int64_t rsid = sqlite3_last_insert_rowid(db);
                    sqlite3_stmt *ri;
                    if (sqlite3_prepare_v2(db,
                            "INSERT INTO channel_routes"
                            " (channel_name, chat_id, session_id, delivery_mode)"
                            " VALUES(?1, ?2, ?3, 'explicit')",
                            -1, &ri, NULL) != SQLITE_OK) { rc = -1; break; }
                    sqlite3_bind_text(ri, 1, rch, -1, SQLITE_STATIC);
                    sqlite3_bind_text(ri, 2, rchat, -1, SQLITE_STATIC);
                    sqlite3_bind_int64(ri, 3, rsid);
                    if (sqlite3_step(ri) != SQLITE_DONE) rc = -1;
                    sqlite3_finalize(ri);
                }
                sqlite3_finalize(ra);
            } else {
                rc = -1;
            }
        }
        if (rc == 0) {
            sqlite3_exec(db, "RELEASE req_changes", NULL, NULL, NULL);
            /* Verify-after-write receipt: report what is NOW in effect from a
             * re-read, never from intent — the agent should not have to trust
             * its own request (or assert success unchecked, as observed). */
            if (detail_out) {
                sqlite3_stmt *rs;
                if (sqlite3_prepare_v2(db, RECEIPT_SQL, -1, &rs, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(rs, 1, args_json, -1, SQLITE_STATIC);
                    sqlite3_bind_text(rs, 2, agent, -1, SQLITE_STATIC);
                    if (sqlite3_step(rs) == SQLITE_ROW) {
                        const char *v = (const char *)sqlite3_column_text(rs, 0);
                        if (v && v[0]) *detail_out = strdup(v);
                    }
                    sqlite3_finalize(rs);
                }
                if (!*detail_out)
                    apply_detail(detail_out, "applied (no effective state to "
                                             "report)");
            }
            /* Written and re-read — but a re-read only proves what the rows
             * say, not that a request now reaches a model (the 2026-08-10
             * incident's whole gap). Documents that move routing get probed
             * for real; a failure reverts to `snapshot` right here, so the
             * agent is never told a broken switch succeeded. */
            char *verdict = NULL;
            if (config_probe_verify(db, agent, args_json, snapshot,
                                    session_id, &verdict) != 0) {
                if (detail_out) {
                    free(*detail_out);
                    *detail_out = verdict;   /* the failure IS the receipt */
                    verdict = NULL;
                }
                rc = -1;
            } else if (verdict && detail_out && *detail_out) {
                size_t need = strlen(*detail_out) + strlen(verdict) + 3;
                char *joined = malloc(need);
                if (joined) {
                    snprintf(joined, need, "%s; %s", *detail_out, verdict);
                    free(*detail_out);
                    *detail_out = joined;
                }
            }
            free(verdict);
        } else {
            sqlite3_exec(db, "ROLLBACK TO req_changes; RELEASE req_changes",
                         NULL, NULL, NULL);
            apply_detail(detail_out, "apply failed (see daemon log)");
        }
    }

    for (size_t i = 0; i < n; i++) { free(lines[i].kind); free(lines[i].value); }
    free(lines);
    free(snapshot);
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
        "provider definition (openrouter, gemini, openai, deepseek, groq, "
        "mistral, cerebras, or a custom name "
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
