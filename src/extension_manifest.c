#define _POSIX_C_SOURCE 200809L
#include "extension_manifest.h"
#include "agent_define.h"
#include "cron.h"
#include "validate.h"
#include "log.h"
#include "skills.h"
#include "templates.h"
#include "util.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── small helpers ──────────────────────────────────────────────── */

static char *xerr(char **out, const char *msg) {
    if (out) *out = strdup(msg);
    return NULL;
}

/* Recursively copy a directory tree (regular files + subdirs only). */
static int copy_tree(const char *src, const char *dst) {
    if (util_mkdir_p(dst) != 0) return -1;
    DIR *d = opendir(src);
    if (!d) return -1;
    int rc = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char sp[PATH_MAX], dp[PATH_MAX];
        snprintf(sp, sizeof(sp), "%s/%s", src, ent->d_name);
        snprintf(dp, sizeof(dp), "%s/%s", dst, ent->d_name);
        struct stat st;
        if (lstat(sp, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (copy_tree(sp, dp) != 0) { rc = -1; break; }
        } else if (S_ISREG(st.st_mode)) {
            if (util_copy_file(sp, dp, 0644) != 0) { rc = -1; break; }
        }
        /* symlinks and other types are skipped */
    }
    closedir(d);
    return rc;
}

/* Resolve the shared-store directory for an extension: <db_dir>/extensions/<name>.
 * Derives <db_dir> from the open DB's file path. Returns 0 on success. */
static int store_dir_for(sqlite3 *db, const char *name, char *out, size_t cap) {
    const char *dbfile = sqlite3_db_filename(db, "main");
    if (!dbfile || !dbfile[0]) return -1;
    char base[PATH_MAX];
    snprintf(base, sizeof(base), "%s", dbfile);
    char *slash = strrchr(base, '/');
    if (slash) *slash = '\0';
    else snprintf(base, sizeof(base), ".");
    snprintf(out, cap, "%s/extensions/%s", base, name);
    return 0;
}

/* Minimal dotted-numeric version compare (a < b), good enough to flag a
 * re-promote downgrade. Non-numeric/missing components compare as 0. */
static int semver_lt(const char *a, const char *b) {
    const char *pa = a, *pb = b;
    while (1) {
        long na = pa ? strtol(pa, (char **)&pa, 10) : 0;
        long nb = pb ? strtol(pb, (char **)&pb, 10) : 0;
        if (na != nb) return na < nb;
        /* Advance past the dot; anything else (non-numeric junk, end of
         * string) terminates its side — guards against an infinite loop on
         * non-numeric versions. */
        int adv = 0;
        if (pa && *pa == '.') { pa++; adv = 1; } else pa = NULL;
        if (pb && *pb == '.') { pb++; adv = 1; } else pb = NULL;
        if (!adv) return 0;
    }
}

/* ── JSON1 query helpers (open DB) ──────────────────────────────── */

/* Run a single-row SELECT json_extract(?1, path); return malloc'd text or NULL. */
static char *json_text(sqlite3 *db, const char *manifest, const char *path) {
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT json_extract(?1, %s)", path);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(st, 1, manifest, -1, SQLITE_STATIC);
    char *res = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) res = strdup(v);
    }
    sqlite3_finalize(st);
    return res;
}

/* Prepare a statement and bind any of the named params present in the SQL. */
static int run_ingest(sqlite3 *db, const char *sql, const char *manifest,
                      const char *name, const char *store, const char *owner) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    int idx;
    if ((idx = sqlite3_bind_parameter_index(st, ":m")) > 0)
        sqlite3_bind_text(st, idx, manifest, -1, SQLITE_STATIC);
    if ((idx = sqlite3_bind_parameter_index(st, ":name")) > 0)
        sqlite3_bind_text(st, idx, name, -1, SQLITE_STATIC);
    if ((idx = sqlite3_bind_parameter_index(st, ":store")) > 0)
        sqlite3_bind_text(st, idx, store, -1, SQLITE_STATIC);
    if ((idx = sqlite3_bind_parameter_index(st, ":owner")) > 0)
        sqlite3_bind_text(st, idx, owner, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE || rc == SQLITE_ROW) ? 0 : -1;
}

/* ── validation ─────────────────────────────────────────────────── */

/* Verify every handler declared at json_each(<arrpath>) names a .qjs file that
 * exists under bundle_dir. Returns 0 if all present. */
static int check_handlers(sqlite3 *db, const char *manifest, const char *bundle_dir,
                          const char *arrpath, char **err_out) {
    char sql[160];
    snprintf(sql, sizeof(sql),
             "SELECT json_extract(value,'$.handler') "
             "FROM json_each(COALESCE(json_extract(?1, %s), '[]'))", arrpath);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, manifest, -1, SQLITE_STATIC);
    int rc = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *h = (const char *)sqlite3_column_text(st, 0);
        if (!h || !h[0]) {
            xerr(err_out, "manifest declares an entry with no handler");
            rc = -1; break;
        }
        size_t hl = strlen(h);
        if (hl < 5 || strcmp(h + hl - 4, ".qjs") != 0) {
            if (err_out) {
                char *m = malloc(hl + 64);
                if (m) { snprintf(m, hl + 64, "handler must end in .qjs: %s", h); *err_out = m; }
            }
            rc = -1; break;
        }
        if (strstr(h, "..") || h[0] == '/') {
            xerr(err_out, "handler path must be a bundle-relative filename");
            rc = -1; break;
        }
        char fp[PATH_MAX];
        snprintf(fp, sizeof(fp), "%s/%s", bundle_dir, h);
        struct stat sb;
        if (stat(fp, &sb) != 0 || !S_ISREG(sb.st_mode)) {
            if (err_out) {
                char *m = malloc(hl + 64);
                if (m) { snprintf(m, hl + 64, "handler file missing: %s", h); *err_out = m; }
            }
            rc = -1; break;
        }
    }
    sqlite3_finalize(st);
    return rc;
}

/* Verify every $.tools[] policy is a shape policy_eval can enforce: an
 * object whose rules (if present) are an array of {match: object, effect:
 * allow|ask|deny}, match values text or array-of-text. policy_eval fails
 * closed on these shapes at runtime (POLICY_ERROR blocks the call), so a
 * bad policy must be rejected HERE, where the author can fix it — not
 * discovered as a bricked tool on its first call. */
static int check_policies(sqlite3 *db, const char *manifest, char **err_out) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT COALESCE(json_extract(t.value,'$.name'),'(unnamed)')"
            " FROM json_each(COALESCE(json_extract(?1,'$.tools'),'[]')) t"
            " WHERE json_extract(t.value,'$.policy') IS NOT NULL AND ("
            "   json_type(t.value,'$.policy')!='object'"
            "   OR (json_type(t.value,'$.policy.rules') IS NOT NULL"
            "       AND json_type(t.value,'$.policy.rules')!='array')"
            "   OR EXISTS (SELECT 1 FROM json_each(t.value,'$.policy.rules') r"
            /* IS NOT (null-safe): an absent match must fail the rule. */
            "       WHERE json_type(r.value,'$.match') IS NOT 'object'"
            "          OR COALESCE(json_extract(r.value,'$.effect'),'')"
            "             NOT IN ('allow','ask','deny')"
            "          OR EXISTS (SELECT 1 FROM json_each(r.value,'$.match') m"
            "              WHERE m.type NOT IN ('text','array')"
            "                 OR (m.type='array' AND EXISTS ("
            "                     SELECT 1 FROM json_each(m.value) e"
            "                     WHERE e.type!='text'))))"
            " ) LIMIT 1", -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, manifest, -1, SQLITE_STATIC);
    int rc = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *n = (const char *)sqlite3_column_text(st, 0);
        if (err_out) {
            size_t len = (n ? strlen(n) : 1) + 160;
            char *m = malloc(len);
            if (m) {
                snprintf(m, len, "tool '%s': invalid policy — needs an object "
                         "whose rules are [{match: object, effect: "
                         "allow|ask|deny}], match values text or [text]",
                         n ? n : "?");
                *err_out = m;
            }
        }
        rc = -1;
    }
    sqlite3_finalize(st);
    return rc;
}

/* Verify every $.config[] entry has an identifier key (no dots — '.' is the
 * namespace separator) and a description. Rows land in the config table as
 * <ext>.<key>, so a bad key must not smuggle a foreign namespace. */
static int check_config(sqlite3 *db, const char *manifest, char **err_out) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT json_extract(value,'$.key'), json_extract(value,'$.description'), "
            "       json_extract(value,'$.default') "
            "FROM json_each(COALESCE(json_extract(?1,'$.config'),'[]'))",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, manifest, -1, SQLITE_STATIC);
    int rc = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *k = (const char *)sqlite3_column_text(st, 0);
        const char *d = (const char *)sqlite3_column_text(st, 1);
        const char *dv = (const char *)sqlite3_column_text(st, 2);
        if (!k || !k[0]) { xerr(err_out, "config entry missing 'key'"); rc = -1; break; }
        for (const char *p = k; *p; p++) {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                  (*p >= '0' && *p <= '9') || *p == '_')) {
                xerr(err_out, "config key must be [A-Za-z0-9_]+ (namespaced as <ext>.<key>)");
                rc = -1; break;
            }
        }
        if (rc != 0) break;
        if (!d || !d[0]) {
            xerr(err_out, "config entry missing 'description' (self-describing knobs only)");
            rc = -1; break;
        }
        /* An egress_hosts default of '*' turns the channel's host pin off
         * entirely (host_match treats a bare '*' as match-any), and the
         * default takes effect with no operator write — so a bundle could
         * un-pin its own egress via a knob the approval summary renders as
         * "1 config key". Naming hosts in a default is fine; disabling the
         * pin is an operator decision, made by setting the value. */
        if (strcmp(k, "egress_hosts") == 0 && dv) {
            for (const char *p = dv; *p; ) {
                while (*p == ',' || *p == ' ' || *p == '\t') p++;
                const char *e = p;
                while (*e && *e != ',') e++;
                const char *t = e;
                while (t > p && (t[-1] == ' ' || t[-1] == '\t')) t--;
                if (t - p == 1 && *p == '*') {
                    xerr(err_out, "egress_hosts default may not be '*' — a manifest "
                                  "may name hosts, but only an operator may unpin egress");
                    rc = -1; break;
                }
                if (!*e) break;
                p = e + 1;
            }
            if (rc != 0) break;
        }
    }
    sqlite3_finalize(st);
    return rc;
}

/* Verify every $.skills[] entry is a safe bundle-relative path to a skill:
 * a directory containing SKILL.md, or a bare .md file — and that its
 * frontmatter parses with a description (the index entry). */
static int check_skills(sqlite3 *db, const char *manifest, const char *bundle_dir,
                        char **err_out) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT value FROM json_each(COALESCE(json_extract(?1,'$.skills'),'[]'))",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, manifest, -1, SQLITE_STATIC);
    int rc = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *s = (const char *)sqlite3_column_text(st, 0);
        if (!s || !s[0] || s[0] == '/' || strstr(s, "..")) {
            xerr(err_out, "skill path must be a bundle-relative path");
            rc = -1; break;
        }
        char fp[PATH_MAX];
        snprintf(fp, sizeof(fp), "%s/%s", bundle_dir, s);
        struct stat sb;
        if (stat(fp, &sb) == 0 && S_ISDIR(sb.st_mode)) {
            size_t l = strlen(fp);
            snprintf(fp + l, sizeof(fp) - l, "/SKILL.md");
        }
        char *name = NULL, *desc = NULL;
        if (skill_frontmatter_parse(fp, &name, &desc) != 0) {
            if (err_out) {
                char *m = malloc(strlen(s) + 80);
                if (m) {
                    snprintf(m, strlen(s) + 80,
                             "skill '%s': missing SKILL.md or no frontmatter description", s);
                    *err_out = m;
                }
            }
            rc = -1; break;
        }
        free(name); free(desc);
    }
    sqlite3_finalize(st);
    return rc;
}

/* Verify every $.agents[] entry is a plausible agent definition: PascalCase
 * name, known sandbox_profile, and an existing bundle-relative
 * system_prompt_file if declared. DB-dependent caps (grants subset, extension
 * visibility) are enforced at install time by agent_definition_validate. */
static int check_agents(sqlite3 *db, const char *manifest, const char *bundle_dir,
                        char **err_out) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT json_extract(value,'$.name'),"
            "       json_extract(value,'$.sandbox_profile'),"
            "       json_extract(value,'$.system_prompt_file') "
            "FROM json_each(COALESCE(json_extract(?1,'$.agents'),'[]'))",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, manifest, -1, SQLITE_STATIC);
    int rc = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *n = (const char *)sqlite3_column_text(st, 0);
        const char *p = (const char *)sqlite3_column_text(st, 1);
        const char *f = (const char *)sqlite3_column_text(st, 2);
        if (!n || !is_valid_agent_name(n)) {
            xerr(err_out, "agents[] entry needs a PascalCase 'name'");
            rc = -1; break;
        }
        if (p && strcmp(p, "host") != 0 && strcmp(p, "trusted") != 0 &&
            strcmp(p, "standard") != 0 && strcmp(p, "restricted") != 0) {
            xerr(err_out, "agents[] sandbox_profile must be host|trusted|standard|restricted");
            rc = -1; break;
        }
        if (f && f[0]) {
            if (f[0] == '/' || strstr(f, "..")) {
                xerr(err_out, "system_prompt_file must be a bundle-relative path");
                rc = -1; break;
            }
            char fp[PATH_MAX];
            snprintf(fp, sizeof(fp), "%s/%s", bundle_dir, f);
            struct stat sb;
            if (stat(fp, &sb) != 0 || !S_ISREG(sb.st_mode)) {
                xerr(err_out, "agents[] system_prompt_file missing from bundle");
                rc = -1; break;
            }
        }
    }
    sqlite3_finalize(st);
    return rc;
}

int extension_manifest_validate(const char *bundle_dir, char **err_out) {
    if (err_out) *err_out = NULL;
    if (!bundle_dir) { xerr(err_out, "no bundle dir"); return -1; }

    char mpath[PATH_MAX];
    snprintf(mpath, sizeof(mpath), "%s/extension.json", bundle_dir);
    size_t mlen = 0;
    char *manifest = util_read_file(mpath, &mlen);
    if (!manifest) { xerr(err_out, "extension.json not found or unreadable"); return -1; }

    /* Use an in-memory DB purely as a JSON1 engine — no schema needed. */
    sqlite3 *jdb = NULL;
    if (sqlite3_open(":memory:", &jdb) != SQLITE_OK) {
        free(manifest);
        if (jdb) sqlite3_close(jdb);
        xerr(err_out, "json engine init failed");
        return -1;
    }

    int rc = -1;
    /* json_valid check */
    {
        sqlite3_stmt *st;
        int ok = 0;
        if (sqlite3_prepare_v2(jdb, "SELECT json_valid(?1)", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, manifest, -1, SQLITE_STATIC);
            if (sqlite3_step(st) == SQLITE_ROW) ok = sqlite3_column_int(st, 0);
            sqlite3_finalize(st);
        }
        if (!ok) { xerr(err_out, "manifest is not valid JSON"); goto out; }
    }

    char *name = json_text(jdb, manifest, "'$.name'");
    if (!name || !name[0]) { free(name); xerr(err_out, "manifest missing 'name'"); goto out; }
    if (strchr(name, '/') || strchr(name, '\\') || strstr(name, "..")) {
        free(name); xerr(err_out, "invalid extension name (no path separators)"); goto out;
    }
    free(name);

    if (check_handlers(jdb, manifest, bundle_dir, "'$.tools'", err_out) != 0) goto out;
    if (check_handlers(jdb, manifest, bundle_dir, "'$.hooks'", err_out) != 0) goto out;
    /* Hook events must be ones the daemon actually dispatches — accepting
     * turnStart/turnEnd (declared in specs/hooks.md, never wired) would
     * register a hook that silently never fires (review-1 F13). */
    {
        sqlite3_stmt *st;
        char *bad = NULL;
        if (sqlite3_prepare_v2(jdb,
                "SELECT json_extract(value,'$.event')"
                " FROM json_each(COALESCE(json_extract(?1,'$.hooks'),'[]'))"
                " WHERE COALESCE(json_extract(value,'$.event'),'') NOT IN"
                "   ('preAdvance','postAdvance','beforeToolCall','afterToolCall')"
                " LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, manifest, -1, SQLITE_STATIC);
            if (sqlite3_step(st) == SQLITE_ROW) {
                const char *v = (const char *)sqlite3_column_text(st, 0);
                bad = strdup(v ? v : "(missing)");
            }
            sqlite3_finalize(st);
        }
        if (bad) {
            if (err_out) {
                size_t n = strlen(bad) + 128;
                char *m = malloc(n);
                if (m) {
                    snprintf(m, n, "hook event '%s' is not dispatched — use "
                             "preAdvance, postAdvance, beforeToolCall, or "
                             "afterToolCall", bad);
                    *err_out = m;
                }
            }
            free(bad);
            goto out;
        }
    }
    if (check_handlers(jdb, manifest, bundle_dir, "'$.scripts'", err_out) != 0) goto out;
    if (check_policies(jdb, manifest, err_out) != 0) goto out;
    if (check_config(jdb, manifest, err_out) != 0) goto out;
    if (check_skills(jdb, manifest, bundle_dir, err_out) != 0) goto out;
    if (check_agents(jdb, manifest, bundle_dir, err_out) != 0) goto out;
    /* channel is a single object, not an array — wrap so check_handlers sees an array */
    {
        char *ch = json_text(jdb, manifest, "'$.channel'");
        if (ch && ch[0]) {
            sqlite3_stmt *st;
            char *chandler = NULL;
            if (sqlite3_prepare_v2(jdb, "SELECT json_extract(?1,'$.handler')", -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_text(st, 1, ch, -1, SQLITE_STATIC);
                if (sqlite3_step(st) == SQLITE_ROW) {
                    const char *v = (const char *)sqlite3_column_text(st, 0);
                    if (v) chandler = strdup(v);
                }
                sqlite3_finalize(st);
            }
            if (chandler && chandler[0]) {
                char fp[PATH_MAX];
                snprintf(fp, sizeof(fp), "%s/%s", bundle_dir, chandler);
                struct stat sb;
                size_t cl = strlen(chandler);
                if (cl < 5 || strcmp(chandler + cl - 4, ".qjs") != 0 ||
                    stat(fp, &sb) != 0 || !S_ISREG(sb.st_mode)) {
                    free(chandler); free(ch);
                    xerr(err_out, "channel handler missing or not a .qjs file");
                    goto out;
                }
            }
            free(chandler);
            /* transports[] must be spelled exactly: its only consumer
             * (manifest_declares_persistent) string-matches 'persistent', so a
             * typo reads as "declares no persistent transport" and silently
             * fails *open* — skipping the WS-capability gate that exists to
             * refuse a channel libcurl cannot actually run. */
            {
                sqlite3_stmt *ts;
                char *badt = NULL;
                /* Shape first: a bare string ("transports": "persistent")
                 * makes json_each error out, which reads as "no rows" — the
                 * same fail-open this check exists to close. */
                if (sqlite3_prepare_v2(jdb,
                        "SELECT 1 WHERE json_type(?1,'$.transports') IS NOT NULL"
                        " AND json_type(?1,'$.transports') <> 'array'",
                        -1, &ts, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(ts, 1, ch, -1, SQLITE_STATIC);
                    int bad_shape = sqlite3_step(ts) == SQLITE_ROW;
                    sqlite3_finalize(ts);
                    if (bad_shape) {
                        free(ch);
                        xerr(err_out, "channel transports must be an array");
                        goto out;
                    }
                }
                if (sqlite3_prepare_v2(jdb,
                        "SELECT value FROM json_each("
                        "  COALESCE(json_extract(?1,'$.transports'),'[]'))"
                        " WHERE value NOT IN ('poll','webhook','persistent') LIMIT 1",
                        -1, &ts, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(ts, 1, ch, -1, SQLITE_STATIC);
                    if (sqlite3_step(ts) == SQLITE_ROW) {
                        const char *v = (const char *)sqlite3_column_text(ts, 0);
                        badt = strdup(v ? v : "(null)");
                    }
                    sqlite3_finalize(ts);
                }
                if (badt) {
                    char m[192];
                    snprintf(m, sizeof(m), "unknown channel transport '%s' — "
                             "use poll, webhook, or persistent", badt);
                    free(badt); free(ch);
                    xerr(err_out, m);
                    goto out;
                }
            }
        }
        free(ch);
    }

    rc = 0;
out:
    free(manifest);
    sqlite3_close(jdb);
    return rc;
}

/* ── install ────────────────────────────────────────────────────── */

int extension_install(sqlite3 *db, const char *bundle_dir,
                      const char *owner_agent, char **err_out) {
    if (err_out) *err_out = NULL;
    if (!db || !bundle_dir) { xerr(err_out, "bad arguments"); return -1; }

    if (extension_manifest_validate(bundle_dir, err_out) != 0)
        return -1;

    char mpath[PATH_MAX];
    snprintf(mpath, sizeof(mpath), "%s/extension.json", bundle_dir);
    char *manifest = util_read_file(mpath, NULL);
    if (!manifest) { xerr(err_out, "extension.json unreadable"); return -1; }

    char *name = json_text(db, manifest, "'$.name'");
    if (!name) { free(manifest); xerr(err_out, "manifest missing 'name'"); return -1; }

    char store[PATH_MAX];
    if (store_dir_for(db, name, store, sizeof(store)) != 0) {
        free(manifest); free(name);
        xerr(err_out, "cannot resolve shared store path");
        return -1;
    }

    /* First-come name ownership (npm-style): a promote may never change the
     * owner of an existing name. Overriding what code fronts a channel is an
     * explicit operator verb (channel swap), never a name seizure. */
    {
        sqlite3_stmt *st;
        char *cur_owner = NULL;
        if (sqlite3_prepare_v2(db,
                "SELECT COALESCE(owner_agent,'') FROM extensions WHERE name=?1",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
            if (sqlite3_step(st) == SQLITE_ROW) {
                const char *v = (const char *)sqlite3_column_text(st, 0);
                cur_owner = strdup(v ? v : "");
            }
            sqlite3_finalize(st);
        }
        if (cur_owner && strcmp(cur_owner, owner_agent ? owner_agent : "") != 0) {
            if (err_out) {
                size_t n = strlen(name) + strlen(cur_owner) + 80;
                char *m = malloc(n);
                if (m) {
                    snprintf(m, n, "extension name '%s' is owned by '%s'; "
                             "promote under a different name", name, cur_owner);
                    *err_out = m;
                }
            }
            free(cur_owner); free(manifest); free(name);
            return -1;
        }
        free(cur_owner);
    }

    /* Copy bundle into the shared, agent-immutable store. */
    if (copy_tree(bundle_dir, store) != 0) {
        free(manifest); free(name);
        xerr(err_out, "failed to copy bundle into shared store");
        return -1;
    }

    int rc = 0;
    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);

    /* Idempotent re-install: clear this extension's prior tool/hook rows. */
    rc |= run_ingest(db, "DELETE FROM tools WHERE extension_name=:name", manifest, name, store, owner_agent);
    rc |= run_ingest(db, "DELETE FROM hooks WHERE extension_name=:name", manifest, name, store, owner_agent);

    /* extensions: upsert, preserving an existing published flag; the owner
     * can never change (takeover is refused above). Log old->new version on
     * re-promote so the lifecycle is visible without a DB query. */
    {
        char *old_version = NULL;
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db, "SELECT version FROM extensions WHERE name=?1",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
            if (sqlite3_step(st) == SQLITE_ROW) {
                const char *v = (const char *)sqlite3_column_text(st, 0);
                old_version = v ? strdup(v) : NULL;
            }
            sqlite3_finalize(st);
        }
        char *new_version = json_text(db, manifest, "'$.version'");
        const char *nv = new_version && new_version[0] ? new_version : "0.0.0";
        if (old_version && strcmp(old_version, nv) != 0) {
            LOG_INFO_("extension_promote: '%s' version %s -> %s", name, old_version, nv);
            if (semver_lt(nv, old_version))
                LOG_WARN_("extension_promote: '%s' downgraded (%s -> %s)", name, old_version, nv);
        }
        free(old_version);
        free(new_version);
    }
    rc |= run_ingest(db,
        "INSERT INTO extensions(name, path, version, owner_agent, published, enabled) "
        "VALUES(:name, :store, COALESCE(json_extract(:m,'$.version'),'0.0.0'), :owner, 0, 1) "
        "ON CONFLICT(name) DO UPDATE SET "
        "  path=excluded.path, version=excluded.version",
        manifest, name, store, owner_agent);

    /* tools: one row per $.tools[] entry; path = <store>/<handler>. */
    rc |= run_ingest(db,
        "INSERT INTO tools(name, extension_name, description, parameters_json, path, "
        "                  agent_name, enabled, policy) "
        "SELECT json_extract(value,'$.name'), :name, json_extract(value,'$.description'), "
        "       json_extract(value,'$.parameters'), :store || '/' || json_extract(value,'$.handler'), "
        "       NULL, 1, json_extract(value,'$.policy') "
        "FROM json_each(COALESCE(json_extract(:m,'$.tools'),'[]')) "
        "WHERE json_extract(value,'$.name') IS NOT NULL",
        manifest, name, store, owner_agent);

    /* hooks: one row per $.hooks[] entry. */
    rc |= run_ingest(db,
        "INSERT INTO hooks(extension_name, event, path, enabled) "
        "SELECT :name, json_extract(value,'$.event'), "
        "       :store || '/' || json_extract(value,'$.handler'), 1 "
        "FROM json_each(COALESCE(json_extract(:m,'$.hooks'),'[]')) "
        "WHERE json_extract(value,'$.event') IS NOT NULL",
        manifest, name, store, owner_agent);

    /* config: registry rows namespaced <ext>.<key>. Same contract as
     * config_registry_sync — default_value/description are code-owned and
     * refreshed on every install; the operator/agent override in `value`
     * is never touched. Keys no longer declared are dropped first. */
    rc |= run_ingest(db,
        "DELETE FROM config WHERE substr(key, 1, length(:name)+1) = :name || '.' "
        "AND key NOT IN (SELECT :name || '.' || json_extract(value,'$.key') "
        "  FROM json_each(COALESCE(json_extract(:m,'$.config'),'[]')))",
        manifest, name, store, owner_agent);
    rc |= run_ingest(db,
        "INSERT INTO config(key, default_value, description, secret, required) "
        "SELECT :name || '.' || json_extract(value,'$.key'), "
        "       COALESCE(json_extract(value,'$.default'), ''), "
        "       json_extract(value,'$.description'), "
        "       COALESCE(json_extract(value,'$.secret'), 0), "
        "       COALESCE(json_extract(value,'$.required'), 0) "
        "FROM json_each(COALESCE(json_extract(:m,'$.config'),'[]')) "
        "WHERE json_extract(value,'$.key') IS NOT NULL "
        "ON CONFLICT(key) DO UPDATE SET default_value=excluded.default_value, "
        "  description=excluded.description, secret=excluded.secret, "
        "  required=excluded.required",
        manifest, name, store, owner_agent);

    /* attach to the owner (published stays 0 until publish). "system" is not
     * an agents row (v31 FK would refuse it): builtin installs stay global —
     * tools carry agent_name NULL and attachment happens per-agent later. */
    if (owner_agent && strcmp(owner_agent, "system") != 0) {
        rc |= run_ingest(db,
            "INSERT OR IGNORE INTO agent_extensions(agent_name, extension_name, enabled) "
            "VALUES(:owner, :name, 1)",
            manifest, name, store, owner_agent);

        /* Grants are the single authorization currency (D1): the promote approval
         * already enumerated these tools, so seed the owner's tool grants here.
         * Attach alone never authorizes — other agents go through request_config. */
        rc |= run_ingest(db,
            "INSERT OR IGNORE INTO grants(agent_name, kind, value, expires_at) "
            "SELECT :owner, 'tool', json_extract(value,'$.name'), NULL "
            "FROM json_each(COALESCE(json_extract(:m,'$.tools'),'[]')) "
            "WHERE json_extract(value,'$.name') IS NOT NULL",
            manifest, name, store, owner_agent);
    }

    /* channel (at most one): joined to the extension by name. */
    {
        char *ch = json_text(db, manifest, "'$.channel'");
        if (ch && ch[0]) {
            /* Always lands in 'draft', even on a re-promote of a channel that
             * was 'active' — the running process keeps going on the old code
             * until --check/--activate (or a daemon restart) picks up the
             * new version. See templates/schema.sql's lifecycle comment.
             * Upsert, not OR REPLACE: operator state on the row
             * (default_agent, prev_extension_name, created_at) must survive
             * a reinstall — install owns only the code-identity columns. */
            rc |= run_ingest(db,
                "INSERT INTO channels(name, extension_name, type, binary_path, status) "
                "VALUES(:name, :name, json_extract(:m,'$.channel.type'), "
                "       :store || '/' || json_extract(:m,'$.channel.handler'), 'draft') "
                "ON CONFLICT(name) DO UPDATE SET "
                "  extension_name=excluded.extension_name, type=excluded.type, "
                "  binary_path=excluded.binary_path, status='draft'",
                manifest, name, store, owner_agent);
            /* Every channel extension has an 'enabled' key, default off —
             * the launch gate reads it (specs/config.md). Declared keys win
             * (INSERT OR IGNORE after the config ingest above). */
            rc |= run_ingest(db,
                "INSERT OR IGNORE INTO config(key, default_value, description) "
                "VALUES(:name || '.enabled', '0', 'Run the ' || :name || ' channel (1 = on)')",
                manifest, name, store, owner_agent);
        }
        free(ch);
    }

    if (sqlite3_exec(db, rc == 0 ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL) != SQLITE_OK)
        rc = -1;

    /* scripts with a schedule seed cron rows (outside the txn — cron_add owns
     * its own insert and computes next_run_at). Routing is refined in Layer 5. */
    if (rc == 0) {
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db,
                "SELECT json_extract(value,'$.name'), json_extract(value,'$.schedule'), "
                "       json_extract(value,'$.handler') "
                "FROM json_each(COALESCE(json_extract(?1,'$.scripts'),'[]')) "
                "WHERE json_extract(value,'$.schedule') IS NOT NULL",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, manifest, -1, SQLITE_STATIC);
            /* WAL safety: this loop writes (cron_add INSERT) mid-iteration of
             * `st`, but safe because json_each() over a bound parameter takes
             * no read snapshot.  If this query ever JOINs a real table,
             * restructure to collect-then-write — see channel_consume_events
             * in src/channel.c. */
            while (sqlite3_step(st) == SQLITE_ROW) {
                const char *sname = (const char *)sqlite3_column_text(st, 0);
                const char *sched = (const char *)sqlite3_column_text(st, 1);
                const char *handler = (const char *)sqlite3_column_text(st, 2);
                if (!sname || !sched || !handler) continue;
                char task[PATH_MAX + 128];
                snprintf(task, sizeof(task),
                         "Run the scheduled extension script '%s': call js_eval with "
                         "filename '%s/%s'.", sname, store, handler);
                cron_add(db, owner_agent, sname, sched, 0, 0, 0, task);
            }
            sqlite3_finalize(st);
        }
    }

    /* agents[]: apply each definition through the shared path
     * (specs/self-configuration.md). Caps are enforced against the promoting
     * agent; the builtin owner 'system' is the operator (no caps). An agent
     * name that already exists is skipped, never overwritten — first-come,
     * which also keeps re-promote idempotent. Runs outside the txn because
     * agent_definition_apply owns its own transaction. */
    if (rc == 0) {
        const char *creator =
            (owner_agent && strcmp(owner_agent, "system") != 0) ? owner_agent : NULL;
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db,
                "SELECT json(value), json_extract(value,'$.name'), "
                "       json_extract(value,'$.system_prompt_file') "
                "FROM json_each(COALESCE(json_extract(?1,'$.agents'),'[]'))",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, manifest, -1, SQLITE_STATIC);
            while (rc == 0 && sqlite3_step(st) == SQLITE_ROW) {
                const char *def = (const char *)sqlite3_column_text(st, 0);
                const char *aname = (const char *)sqlite3_column_text(st, 1);
                const char *pfile = (const char *)sqlite3_column_text(st, 2);
                if (!def || !aname) continue;
                sqlite3_stmt *ck;
                int exists = 0;
                if (sqlite3_prepare_v2(db, "SELECT 1 FROM agents WHERE name=?1",
                                       -1, &ck, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(ck, 1, aname, -1, SQLITE_STATIC);
                    exists = (sqlite3_step(ck) == SQLITE_ROW);
                    sqlite3_finalize(ck);
                }
                if (exists) continue;
                /* Resolve system_prompt_file from the installed store copy. */
                char *resolved = NULL;
                if (pfile && pfile[0]) {
                    char fp[2*PATH_MAX];
                    snprintf(fp, sizeof(fp), "%s/%s", store, pfile);
                    char *prompt = util_read_file(fp, NULL);
                    sqlite3_stmt *js;
                    if (prompt && sqlite3_prepare_v2(db,
                            "SELECT json_remove(json_set(?1,'$.system_prompt',"
                            " COALESCE(json_extract(?1,'$.system_prompt'),?2)),"
                            " '$.system_prompt_file')", -1, &js, NULL) == SQLITE_OK) {
                        sqlite3_bind_text(js, 1, def, -1, SQLITE_STATIC);
                        sqlite3_bind_text(js, 2, prompt, -1, SQLITE_STATIC);
                        if (sqlite3_step(js) == SQLITE_ROW) {
                            const char *v = (const char *)sqlite3_column_text(js, 0);
                            if (v) resolved = strdup(v);
                        }
                        sqlite3_finalize(js);
                    }
                    free(prompt);
                }
                char *aerr = NULL;
                if (agent_definition_apply(db, resolved ? resolved : def,
                                           creator, NULL, &aerr) != 0) {
                    if (err_out && !*err_out) {
                        size_t n = strlen(aname) + (aerr ? strlen(aerr) : 8) + 32;
                        char *m = malloc(n);
                        if (m) {
                            snprintf(m, n, "agent '%s': %s", aname,
                                     aerr ? aerr : "apply failed");
                            *err_out = m;
                        }
                    }
                    rc = -1;
                }
                free(aerr);
                free(resolved);
            }
            sqlite3_finalize(st);
        }
    }

    if (rc != 0 && err_out && !*err_out)
        *err_out = strdup("manifest ingest failed");

    free(manifest);
    free(name);
    return rc;
}

/* ── builtin (system-owned) extensions ──────────────────────────── */

static int write_text_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    int rc = (fputs(content, f) >= 0) ? 0 : -1;
    if (fclose(f) != 0) rc = -1;
    return rc;
}

/* PATH_MAX-sized buffers make gcc's -Wformat-truncation flag these snprintfs
 * as possibly truncating; the inputs never approach that length. Same guard
 * as install.c; clang doesn't have this warning at all. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
typedef struct { const char *rel; const char *body; } BuiltinFile;

typedef struct {
    const char *name;
    const BuiltinFile *files;
    size_t nfiles;
} BuiltinBundle;

/* True when an extensions row of this name exists and is NOT system-owned —
 * belt-and-suspenders: the takeover guard in extension_install already
 * refuses this (unreachable in practice), but never fight a foreign row. */
static int builtin_row_foreign(sqlite3 *db, const char *name) {
    sqlite3_stmt *s;
    int foreign = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT COALESCE(owner_agent,'') FROM extensions WHERE name=?1",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const char *o = (const char *)sqlite3_column_text(s, 0);
            foreign = (o && strcmp(o, "system") != 0);
        }
        sqlite3_finalize(s);
    }
    return foreign;
}

/* Stage a builtin bundle beside the store and install it as 'system'.
 * The store dir is the bundle's final home, so installing from a sibling
 * staging dir avoids copy_tree src==dst. Bundle code ships in the binary;
 * the files on disk are a cache, rewritten on every start so a binary
 * upgrade can't leave a stale template running against a newer C loop. */
static int builtin_stage_install(sqlite3 *db, const char *base,
                                 const BuiltinBundle *b) {
    char staging[2*PATH_MAX];
    snprintf(staging, sizeof(staging), "%s/extensions/.%s.staging",
             base, b->name);
    if (util_mkdir_p(staging) != 0) return -1;

    int rc = 0;
    for (size_t i = 0; i < b->nfiles && rc == 0; i++) {
        char fp[3*PATH_MAX];
        snprintf(fp, sizeof(fp), "%s/%s", staging, b->files[i].rel);
        char *sl = strrchr(fp, '/');
        *sl = '\0';
        rc |= util_mkdir_p(fp);
        *sl = '/';
        rc |= write_text_file(fp, b->files[i].body);
    }

    char *err = NULL;
    if (rc == 0) rc = extension_install(db, staging, "system", &err);
    if (rc != 0)
        LOG_ERROR_("builtin extension '%s' install failed: %s",
                   b->name, err ? err : "write failed");
    free(err);

    /* Cleanup: unlink each file, then walk its parent dirs up to the
     * staging root (rmdir fails harmlessly on non-empty). */
    for (size_t i = 0; i < b->nfiles; i++) {
        char fp[3*PATH_MAX];
        snprintf(fp, sizeof(fp), "%s/%s", staging, b->files[i].rel);
        unlink(fp);
        char *sl;
        while ((sl = strrchr(fp, '/')) && (size_t)(sl - fp) > strlen(staging)) {
            *sl = '\0';
            rmdir(fp);
        }
    }
    rmdir(staging);

    if (rc == 0) {
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db,
                "UPDATE extensions SET published=1 WHERE name=?1",
                -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_text(s, 1, b->name, -1, SQLITE_STATIC);
            sqlite3_step(s);
            sqlite3_finalize(s);
        }
    }
    return rc;
}

/* Install a builtin *channel* bundle, preserving the channel's trust status
 * across the reinstall. install's channels upsert deliberately lands 'draft'
 * (the agent re-promote path) but leaves every other column alone; only this
 * wrapper restores the prior status so a restart never demotes an active
 * channel. Builtin bundles are shipped code, already past the trust gate — a
 * fresh install is born 'active'. Whether it *runs* is the separate
 * <ext>.enabled config key (specs/config.md). */
static int install_channel_builtin(sqlite3 *db, const char *base, const char *name,
                                   const BuiltinFile *files, size_t nfiles) {
    if (builtin_row_foreign(db, name)) return 0;   /* never fight a foreign row */

    sqlite3_stmt *s;
    char *status = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT status FROM channels WHERE name=?1",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(s, 0);
            status = strdup(v ? v : "draft");
        }
        sqlite3_finalize(s);
    }

    BuiltinBundle b = { name, files, nfiles };
    int rc = builtin_stage_install(db, base, &b);
    if (rc == 0 && sqlite3_prepare_v2(db,
            "UPDATE channels SET status=?1 WHERE name=?2",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, status ? status : "active", -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, name, -1, SQLITE_STATIC);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
    free(status);
    return rc;
}

int extension_install_builtin(sqlite3 *db, const char *db_path) {
    if (!db || !db_path) return -1;

    char base[PATH_MAX];
    snprintf(base, sizeof(base), "%s", db_path);
    char *sl = strrchr(base, '/');
    if (sl) *sl = '\0';
    else return -1;

    static const BuiltinFile telegram_files[] = {
        { "channel.qjs",    TPL_CHANNEL_TELEGRAM_QJS },
        { "telegram.json",  TPL_CHANNEL_TELEGRAM_JSON },
        { "extension.json", TPL_CHANNEL_TELEGRAM_MANIFEST_JSON },
    };
    static const BuiltinFile discord_files[] = {
        { "channel.qjs",    TPL_CHANNEL_DISCORD_QJS },
        { "discord.json",   TPL_CHANNEL_DISCORD_JSON },
        { "extension.json", TPL_CHANNEL_DISCORD_MANIFEST_JSON },
    };
    static const BuiltinFile docs_files[] = {
        { "extension.json", TPL_DOCS_MANIFEST_JSON },
        { "skills/configuring-cclaw/SKILL.md",    TPL_DOCS_CONFIGURING_CCLAW_MD },
        { "skills/extending-cclaw/SKILL.md",      TPL_DOCS_EXTENDING_CCLAW_MD },
        { "skills/creating-agents/SKILL.md",      TPL_DOCS_CREATING_AGENTS_MD },
        { "skills/cclaw-agents/SKILL.md",         TPL_DOCS_CCLAW_AGENTS_MD },
        { "skills/cclaw-channels/SKILL.md",       TPL_DOCS_CCLAW_CHANNELS_MD },
        { "skills/cclaw-secrets-memory/SKILL.md", TPL_DOCS_CCLAW_SECRETS_MEMORY_MD },
        { "skills/backup-restore/SKILL.md",       TPL_DOCS_BACKUP_RESTORE_MD },
    };

    int rc = 0;

    /* ── channel builtins (status save/restore across reinstall) ── */
    rc |= install_channel_builtin(db, base, "telegram", telegram_files,
                                  sizeof(telegram_files)/sizeof(*telegram_files));
    rc |= install_channel_builtin(db, base, "discord", discord_files,
                                  sizeof(discord_files)/sizeof(*discord_files));

    /* ── cclaw-docs (skills-only self-documentation) ── */
    if (!builtin_row_foreign(db, "cclaw-docs")) {
        BuiltinBundle docs = { "cclaw-docs", docs_files,
                               sizeof(docs_files)/sizeof(*docs_files) };
        int drc = builtin_stage_install(db, base, &docs);
        if (drc == 0) {
            /* Backfill: agents created before this build get the docs too;
             * agent_definition_apply handles new agents via
             * agent_default_extensions. Detachable per-agent like any
             * extension — but a detach is a DELETE, which this INSERT OR
             * IGNORE would undo on restart, so respect an explicit opt-out
             * recorded as enabled=0 instead. */
            sqlite3_exec(db,
                "INSERT OR IGNORE INTO agent_extensions(agent_name, extension_name, enabled)"
                " SELECT name, 'cclaw-docs', 1 FROM agents",
                NULL, NULL, NULL);
        }
        rc |= drc;
    }

    return rc;
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
