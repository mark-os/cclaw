/* Agent-definition schema: validate + apply (specs/self-configuration.md).
 * All three declaration paths (disk seed, create_agent, manifest agents[])
 * funnel through here so creation caps and side effects cannot diverge. */
#define _POSIX_C_SOURCE 200809L
#include "agent_define.h"
#include "agent_config.h"
#include "config_registry.h"
#include "cron.h"
#include "db.h"
#include "util.h"
#include "validate.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(char **err, const char *msg) {
    if (err) *err = strdup(msg);
    return -1;
}

static int failf(char **err, const char *fmt, const char *a, const char *b) {
    if (err) {
        size_t n = strlen(fmt) + (a ? strlen(a) : 0) + (b ? strlen(b) : 0) + 1;
        char *m = malloc(n);
        if (m) { snprintf(m, n, fmt, a ? a : "", b ? b : ""); *err = m; }
    }
    return -1;
}

/* Single-value json_extract over the bound definition. Heap or NULL. */
static char *jext(sqlite3 *db, const char *json, const char *path) {
    char sql[96];
    snprintf(sql, sizeof(sql), "SELECT json_extract(?1, '%s')", path);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC);
    char *res = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) res = strdup(v);
    }
    sqlite3_finalize(st);
    return res;
}

/* Looseness order host > standard > restricted; -1 = unknown. */
static int profile_rank(const char *p) {
    if (!p) return -1;
    if (strcmp(p, "host") == 0) return 2;
    if (strcmp(p, "standard") == 0) return 1;
    if (strcmp(p, "restricted") == 0) return 0;
    return -1;
}

static char *agent_profile(sqlite3 *db, const char *name) {
    sqlite3_stmt *st;
    char *res = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT COALESCE(sandbox_profile,'standard') FROM agents WHERE name=?1",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(st, 0);
            if (v) res = strdup(v);
        }
        sqlite3_finalize(st);
    }
    return res ? res : strdup("standard");
}

static int agent_exists(sqlite3 *db, const char *name) {
    sqlite3_stmt *st;
    int found = 0;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM agents WHERE name=?1",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        found = (sqlite3_step(st) == SQLITE_ROW);
        sqlite3_finalize(st);
    }
    return found;
}

/* Declared grants live at $.grants.{tools,hosts,read_paths,write_paths};
 * the grants-table kind for each. */
static const struct { const char *path; const char *kind; } GRANT_KINDS[] = {
    { "$.grants.tools",       "tool" },
    { "$.grants.hosts",       "host" },
    { "$.grants.read_paths",  "read_path" },
    { "$.grants.write_paths", "write_path" },
};
#define GRANT_KIND_COUNT (sizeof(GRANT_KINDS) / sizeof(GRANT_KINDS[0]))

/* Iterate one declared grant list; per value either subset-check against
 * creator (apply==0) or insert (apply==1, expires_at 0 = permanent).
 * json_each over a bound parameter pins no read snapshot, so the insert
 * mid-iteration is safe (see memory_blocks_seed). */
static int grants_walk(sqlite3 *db, const char *json, const char *target,
                       const char *creator, int apply, size_t ki, char **err) {
    char sql[128];
    snprintf(sql, sizeof(sql),
             "SELECT value FROM json_each(COALESCE(json_extract(?1,'%s'),'[]'))",
             GRANT_KINDS[ki].path);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return fail(err, "grants query failed");
    sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC);
    int rc = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (!v || !v[0]) { rc = fail(err, "empty grant value"); break; }
        if (!apply) {
            /* Refuse at validate so the definition fails whole, with the
             * reason — never leave a half-applied agent behind. */
            if (grant_path_hits_db(db, GRANT_KINDS[ki].kind, v)) {
                rc = failf(err, "grant %s:%s covers cclaw.db — the database is "
                           "never reachable from a sandboxed tool, and no grant "
                           "can change that (use db_query)",
                           GRANT_KINDS[ki].kind, v);
                break;
            }
            if (creator && !grants_contains(db, creator, GRANT_KINDS[ki].kind, v)) {
                rc = failf(err, "grant %s:%s exceeds creator's grants — "
                           "request it for yourself first (request_config "
                           "request_changes), then retry",
                           GRANT_KINDS[ki].kind, v);
                break;
            }
        } else {
            if (agent_config_grant(db, target, GRANT_KINDS[ki].kind, v, 0) != 0) {
                rc = fail(err, "grant insert failed");
                break;
            }
        }
    }
    sqlite3_finalize(st);
    return rc;
}

/* Check every $.extensions[] entry is published or owned by creator
 * (creator NULL = operator: only existence is required). */
static int extensions_check(sqlite3 *db, const char *json, const char *creator,
                            char **err) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT value FROM json_each(COALESCE(json_extract(?1,'$.extensions'),'[]'))",
            -1, &st, NULL) != SQLITE_OK)
        return fail(err, "extensions query failed");
    sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC);
    int rc = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *e = (const char *)sqlite3_column_text(st, 0);
        if (!e || !e[0]) { rc = fail(err, "empty extension name"); break; }
        sqlite3_stmt *ck;
        int ok = 0;
        if (sqlite3_prepare_v2(db,
                "SELECT 1 FROM extensions WHERE name=?1 AND"
                " (published=1 OR ?2 IS NULL OR owner_agent=?2)",
                -1, &ck, NULL) == SQLITE_OK) {
            sqlite3_bind_text(ck, 1, e, -1, SQLITE_STATIC);
            if (creator) sqlite3_bind_text(ck, 2, creator, -1, SQLITE_STATIC);
            else sqlite3_bind_null(ck, 2);
            ok = (sqlite3_step(ck) == SQLITE_ROW);
            sqlite3_finalize(ck);
        }
        if (!ok) {
            rc = failf(err, "extension '%s' not found, or neither published "
                       "nor owned by the creator%s", e, "");
            break;
        }
    }
    sqlite3_finalize(st);
    return rc;
}

/* Creator cap on a profile value (validate path). */
static int profile_capped(sqlite3 *db, const char *profile, const char *creator,
                          char **err) {
    if (!profile) return 0;
    int r = profile_rank(profile);
    if (r < 0)
        return failf(err, "invalid sandbox_profile '%s' (host|standard|restricted)%s",
                     profile, "");
    if (creator) {
        char *cp = agent_profile(db, creator);
        int cr = profile_rank(cp);
        if (r > cr) {
            failf(err, "sandbox_profile '%s' looser than creator's '%s'", profile, cp);
            free(cp);
            return -1;
        }
        free(cp);
    }
    return 0;
}

/* Validate name/profile/grants/extensions/clone_from; no writes.
 * name_out (optional): heap copy of $.name on success. */
static int validate_inner(sqlite3 *db, const char *json, const char *creator,
                          char **name_out, char **err) {
    if (!db || !json) return fail(err, "no definition");

    sqlite3_stmt *st;
    int ok = 0;
    if (sqlite3_prepare_v2(db, "SELECT json_valid(?1)", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) ok = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    if (!ok) return fail(err, "definition is not valid JSON");

    char *name = jext(db, json, "$.name");
    if (!name || !name[0]) { free(name); return fail(err, "definition missing 'name'"); }
    if (!is_valid_agent_name(name)) {
        free(name);
        return fail(err, "agent name must be PascalCase: uppercase first letter, "
                    "letters and digits only, max 63 chars");
    }
    if (agent_exists(db, name)) {
        failf(err, "agent '%s' already exists%s", name, "");
        free(name);
        return -1;
    }

    int rc = 0;
    char *profile = jext(db, json, "$.sandbox_profile");
    rc = profile_capped(db, profile, creator, err);
    free(profile);

    char *clone = NULL;
    if (rc == 0) {
        clone = jext(db, json, "$.clone_from");
        if (clone && clone[0]) {
            if (!agent_exists(db, clone)) {
                rc = failf(err, "clone_from agent '%s' does not exist%s", clone, "");
            } else if (creator) {
                /* Cloning must not launder capabilities: source profile and
                 * live grants are capped like declared ones. */
                char *sp = agent_profile(db, clone);
                rc = profile_capped(db, sp, creator, err);
                free(sp);
                if (rc == 0) {
                    sqlite3_stmt *gs;
                    if (sqlite3_prepare_v2(db,
                            "SELECT kind, value FROM grants WHERE agent_name=?1"
                            " AND (expires_at IS NULL OR expires_at > unixepoch())",
                            -1, &gs, NULL) == SQLITE_OK) {
                        sqlite3_bind_text(gs, 1, clone, -1, SQLITE_STATIC);
                        while (sqlite3_step(gs) == SQLITE_ROW) {
                            const char *k = (const char *)sqlite3_column_text(gs, 0);
                            const char *v = (const char *)sqlite3_column_text(gs, 1);
                            if (k && v && !grants_contains(db, creator, k, v)) {
                                rc = failf(err, "clone_from grant %s:%s exceeds "
                                           "creator's grants — request it for "
                                           "yourself first (request_config "
                                           "request_changes), then retry", k, v);
                                break;
                            }
                        }
                        sqlite3_finalize(gs);
                    }
                }
            }
        }
    }
    free(clone);

    for (size_t i = 0; rc == 0 && i < GRANT_KIND_COUNT; i++)
        rc = grants_walk(db, json, NULL, creator, 0, i, err);
    if (rc == 0)
        rc = extensions_check(db, json, creator, err);

    if (rc == 0 && name_out) { *name_out = name; name = NULL; }
    free(name);
    return rc;
}

int agent_definition_validate(sqlite3 *db, const char *json,
                              const char *creator, char **err) {
    if (err) *err = NULL;
    return validate_inner(db, json, creator, NULL, err);
}

/* Shared apply tail (create and update): overlay declared scalar fields onto
 * the agents row (absent = keep), add declared grants, and re-stamp the
 * approval gate on approval-listed tools. */
static int apply_fields_and_grants(sqlite3 *db, const char *name,
                                   const char *json, char **err) {
    sqlite3_stmt *st;
    int rc = 0;
    const char *osql =
        "UPDATE agents SET"
        " description   = COALESCE(json_extract(?2,'$.description'), description),"
        " system_prompt = COALESCE(json_extract(?2,'$.system_prompt'), system_prompt),"
        " primary_model = COALESCE(json_extract(?2,'$.primary_model'), primary_model),"
        " secondary_model = COALESCE(json_extract(?2,'$.secondary_model'), secondary_model),"
        " sandbox_profile = COALESCE(json_extract(?2,'$.sandbox_profile'), sandbox_profile),"
        " max_iterations  = COALESCE(json_extract(?2,'$.max_iterations'), max_iterations),"
        " shell_timeout   = COALESCE(json_extract(?2,'$.shell_timeout'), shell_timeout)"
        " WHERE name=?1";
    if (sqlite3_prepare_v2(db, osql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, json, -1, SQLITE_STATIC);
        if (sqlite3_step(st) != SQLITE_DONE) rc = -1;
        sqlite3_finalize(st);
    } else rc = -1;

    for (size_t i = 0; rc == 0 && i < GRANT_KIND_COUNT; i++)
        rc = grants_walk(db, json, name, NULL, 1, i, err);

    /* Approval-required tools keep their gate however they were granted. */
    if (rc == 0) {
        char *approval_list = config_get(db, "agent_approval_tools");
        if (approval_list) {
            if (sqlite3_prepare_v2(db,
                    "UPDATE grants SET approval_mode='always' WHERE agent_name=?1"
                    " AND kind='tool' AND value IN (SELECT value FROM json_each(?2))",
                    -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
                sqlite3_bind_text(st, 2, approval_list, -1, SQLITE_STATIC);
                sqlite3_step(st);
                sqlite3_finalize(st);
            }
            free(approval_list);
        }
    }
    return rc;
}

int agent_definition_apply(sqlite3 *db, const char *json, const char *creator,
                           const char *agents_dir, char **err) {
    if (err) *err = NULL;
    char *name = NULL;
    if (validate_inner(db, json, creator, &name, err) != 0)
        return -1;

    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        free(name);
        return fail(err, "cannot begin transaction");
    }

    int rc = 0;
    char *clone = jext(db, json, "$.clone_from");
    sqlite3_stmt *st;

    if (clone && clone[0]) {
        /* Copy row, grants, and extension attachments; overlay below.
         * created_by is the creator, never the source's — provenance
         * doesn't clone. */
        static const char *copies[] = {
            ("INSERT INTO agents(name, created_by, primary_model, secondary_model,"
             " system_prompt, description, max_iterations, max_output_tokens,"
             " shell_timeout, shell_path, sandbox_profile)"
             " SELECT ?1, ?3, primary_model, secondary_model, system_prompt, description,"
             "  max_iterations, max_output_tokens, shell_timeout, shell_path,"
             "  sandbox_profile FROM agents WHERE name=?2"),
            ("INSERT OR IGNORE INTO grants(agent_name, kind, value, approval_mode, expires_at)"
             " SELECT ?1, kind, value, approval_mode, expires_at FROM grants"
             " WHERE agent_name=?2"),
            ("INSERT OR IGNORE INTO agent_extensions(agent_name, extension_name, enabled)"
             " SELECT ?1, extension_name, enabled FROM agent_extensions WHERE agent_name=?2"),
        };
        for (size_t i = 0; rc == 0 && i < 3; i++) {
            if (sqlite3_prepare_v2(db, copies[i], -1, &st, NULL) != SQLITE_OK) { rc = -1; break; }
            sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 2, clone, -1, SQLITE_STATIC);
            if (i == 0) {
                if (creator) sqlite3_bind_text(st, 3, creator, -1, SQLITE_STATIC);
                else sqlite3_bind_null(st, 3);
            }
            if (sqlite3_step(st) != SQLITE_DONE) rc = -1;
            sqlite3_finalize(st);
        }
    } else {
        if (sqlite3_prepare_v2(db, "INSERT INTO agents(name, created_by) VALUES(?1,?2)",
                               -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
            if (creator) sqlite3_bind_text(st, 2, creator, -1, SQLITE_STATIC);
            else sqlite3_bind_null(st, 2);
            if (sqlite3_step(st) != SQLITE_DONE) rc = -1;
            sqlite3_finalize(st);
        } else rc = -1;
    }

    /* Baseline grants (skip for clones — the copied set is the baseline),
     * then declared fields + grants + approval gate. */
    if (rc == 0 && !(clone && clone[0]))
        agent_grant_defaults(db, name);
    if (rc == 0)
        rc = apply_fields_and_grants(db, name, json, err);

    /* Declared + default extension attachments. */
    if (rc == 0) {
        if (sqlite3_prepare_v2(db,
                "INSERT OR IGNORE INTO agent_extensions(agent_name, extension_name, enabled)"
                " SELECT ?1, value, 1 FROM json_each(COALESCE(json_extract(?2,'$.extensions'),'[]'))",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 2, json, -1, SQLITE_STATIC);
            if (sqlite3_step(st) != SQLITE_DONE) rc = -1;
            sqlite3_finalize(st);
        } else rc = -1;
    }
    if (rc == 0) {
        char *defexts = config_get(db, "agent_default_extensions");
        if (defexts) {
            if (sqlite3_prepare_v2(db,
                    "INSERT OR IGNORE INTO agent_extensions(agent_name, extension_name, enabled)"
                    " SELECT ?1, value, 1 FROM json_each(?2)"
                    " WHERE value IN (SELECT name FROM extensions)",
                    -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
                sqlite3_bind_text(st, 2, defexts, -1, SQLITE_STATIC);
                sqlite3_step(st);
                sqlite3_finalize(st);
            }
            free(defexts);
        }
    }

    if (rc == 0)
        memory_blocks_seed(db, name, json);

    /* Every agent gets a disabled 'heartbeat' bare-wake job it can later
     * enable (the seeding infrastructure is the point, not the cadence).
     * Best-effort: a seed failure must not roll back agent creation. */
    if (rc == 0)
        cron_seed_heartbeat(db, name);

    if (sqlite3_exec(db, rc == 0 ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL) != SQLITE_OK)
        rc = -1;

    if (rc == 0 && agents_dir && agents_dir[0]) {
        char ws[PATH_MAX];
        snprintf(ws, sizeof(ws), "%s/%s/workspace", agents_dir, name);
        if (util_mkdir_p(ws) != 0)
            /* Non-fatal: workspace_init recreates on first use. */
            rc = 0;
    }

    if (rc != 0 && err && !*err) *err = strdup("agent definition apply failed");
    free(clone);
    free(name);
    return rc;
}

/* ── update: overlay an existing agent (specs/self-configuration.md) ──
 * Same caps as creation, applied against the caller; authorization is
 * creator-or-self (agents.created_by). Grants are additive-only — removing
 * a child's grant is an operator act. Self grant expansion via this path is
 * a no-op by construction (grants ⊆ caller's own), so escalation stays
 * exclusively with request_changes. */

/* Absent field = keep, so a typo'd key would silently no-op — reject
 * anything outside the whitelist instead. */
#define UPDATE_KEYS_SQL \
    "('name','description','system_prompt','primary_model','secondary_model'," \
    "'sandbox_profile','max_iterations','shell_timeout','grants','reason')"

static int update_validate_inner(sqlite3 *db, const char *json,
                                 const char *caller, char **err) {
    if (!db || !json) return fail(err, "no update document");
    if (!caller || !caller[0]) return fail(err, "no calling agent");

    sqlite3_stmt *st;
    int ok = 0;
    if (sqlite3_prepare_v2(db, "SELECT json_valid(?1)", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) ok = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    if (!ok) return fail(err, "update document is not valid JSON");

    char *name = jext(db, json, "$.name");
    if (!name || !name[0]) { free(name); return fail(err, "update missing 'name'"); }

    /* Authorization: the target must exist and be the caller or its child. */
    int authorized = -1;
    if (sqlite3_prepare_v2(db,
            "SELECT (name=?2 OR created_by=?2) FROM agents WHERE name=?1",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, caller, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) authorized = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    if (authorized < 0) {
        failf(err, "agent '%s' does not exist%s", name, "");
        free(name); return -1;
    }
    if (!authorized) {
        failf(err, "not authorized to update '%s' — only the agent itself "
              "or its creator may%s", name, "");
        free(name); return -1;
    }
    free(name);

    /* Shape checks, one query each — typo-hostile like request_changes. */
    static const struct { const char *sql; const char *msg; } checks[] = {
        { "SELECT key FROM json_each(?1) WHERE key NOT IN " UPDATE_KEYS_SQL,
          "unknown field '%s' — absent fields keep their value, so a typo "
          "would silently change nothing%s" },
        { "SELECT key FROM json_each(?1) WHERE key IN"
          " ('max_iterations','shell_timeout')"
          " AND (type!='integer' OR atom<=0)",
          "'%s' must be a positive integer%s" },
        { "SELECT key FROM json_each(?1) WHERE key IN"
          " ('description','system_prompt','primary_model','secondary_model',"
          "  'sandbox_profile','reason') AND type!='text'",
          "'%s' must be a string%s" },
        { "SELECT atom FROM json_each(?1) je WHERE key IN"
          " ('primary_model','secondary_model')"
          " AND NOT EXISTS (SELECT 1 FROM models m"
          "                 WHERE m.id=je.atom OR m.model=je.atom)",
          "model '%s' does not resolve to a registered model — define its "
          "provider first (request_config request_changes)%s" },
        { "SELECT 1 FROM json_each(?1)"
          " WHERE key NOT IN ('name','reason') LIMIT 1",
          NULL /* inverted: no row = nothing to update */ },
    };
    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
        if (sqlite3_prepare_v2(db, checks[i].sql, -1, &st, NULL) != SQLITE_OK)
            return fail(err, "update validation query failed");
        sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC);
        int row = (sqlite3_step(st) == SQLITE_ROW);
        char *offender = NULL;
        if (row) {
            const char *v = (const char *)sqlite3_column_text(st, 0);
            if (v) offender = strdup(v);
        }
        sqlite3_finalize(st);
        if (checks[i].msg == NULL) {
            free(offender);
            if (!row) return fail(err, "nothing to update — set at least one field");
        } else if (row) {
            failf(err, checks[i].msg, offender ? offender : "?", "");
            free(offender);
            return -1;
        } else {
            free(offender);
        }
    }

    /* Caps, both applied against the caller (creator-or-self alike). */
    char *profile = jext(db, json, "$.sandbox_profile");
    int rc = profile_capped(db, profile, caller, err);
    free(profile);
    for (size_t i = 0; rc == 0 && i < GRANT_KIND_COUNT; i++)
        rc = grants_walk(db, json, NULL, caller, 0, i, err);
    return rc;
}

int agent_definition_update_validate(sqlite3 *db, const char *json,
                                     const char *caller, char **err) {
    if (err) *err = NULL;
    return update_validate_inner(db, json, caller, err);
}

int agent_definition_update_apply(sqlite3 *db, const char *json,
                                  const char *caller, char **err) {
    if (err) *err = NULL;
    /* Re-validate at apply: authorization or caps may have shifted between
     * park and approval (rename, expired grants). */
    if (update_validate_inner(db, json, caller, err) != 0)
        return -1;

    char *name = jext(db, json, "$.name");
    if (!name) return fail(err, "update missing 'name'");

    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        free(name);
        return fail(err, "cannot begin transaction");
    }

    int rc = apply_fields_and_grants(db, name, json, err);

    if (sqlite3_exec(db, rc == 0 ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL) != SQLITE_OK)
        rc = -1;
    if (rc != 0 && err && !*err) *err = strdup("agent update apply failed");
    free(name);
    return rc;
}
