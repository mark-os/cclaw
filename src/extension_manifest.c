#define _POSIX_C_SOURCE 200809L
#include "extension_manifest.h"
#include "cron.h"
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

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[rd] = '\0';
    if (out_len) *out_len = rd;
    return buf;
}

static int mkdir_p(const char *path) {
    char buf[PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(buf)) return -1;
    memcpy(buf, path, len + 1);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int copy_file(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { close(in); return -1; }
    char buf[8192];
    ssize_t n;
    int rc = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w <= 0) { rc = -1; goto done; }
            off += w;
        }
    }
    if (n < 0) rc = -1;
done:
    close(in);
    close(out);
    return rc;
}

/* Recursively copy a directory tree (regular files + subdirs only). */
static int copy_tree(const char *src, const char *dst) {
    if (mkdir_p(dst) != 0) return -1;
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
            if (copy_file(sp, dp) != 0) { rc = -1; break; }
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

int extension_manifest_validate(const char *bundle_dir, char **err_out) {
    if (err_out) *err_out = NULL;
    if (!bundle_dir) { xerr(err_out, "no bundle dir"); return -1; }

    char mpath[PATH_MAX];
    snprintf(mpath, sizeof(mpath), "%s/extension.json", bundle_dir);
    size_t mlen = 0;
    char *manifest = read_file(mpath, &mlen);
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
    if (check_handlers(jdb, manifest, bundle_dir, "'$.scripts'", err_out) != 0) goto out;
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
    char *manifest = read_file(mpath, NULL);
    if (!manifest) { xerr(err_out, "extension.json unreadable"); return -1; }

    char *name = json_text(db, manifest, "'$.name'");
    if (!name) { free(manifest); xerr(err_out, "manifest missing 'name'"); return -1; }

    char store[PATH_MAX];
    if (store_dir_for(db, name, store, sizeof(store)) != 0) {
        free(manifest); free(name);
        xerr(err_out, "cannot resolve shared store path");
        return -1;
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

    /* extensions: upsert, preserving an existing published flag. */
    rc |= run_ingest(db,
        "INSERT INTO extensions(name, path, version, owner_agent, published, builtin, enabled) "
        "VALUES(:name, :store, COALESCE(json_extract(:m,'$.version'),'0.0.0'), :owner, 0, 0, 1) "
        "ON CONFLICT(name) DO UPDATE SET "
        "  path=excluded.path, version=excluded.version, owner_agent=excluded.owner_agent",
        manifest, name, store, owner_agent);

    /* tools: one row per $.tools[] entry; path = <store>/<handler>. */
    rc |= run_ingest(db,
        "INSERT INTO tools(name, extension_name, description, parameters_json, path, "
        "                  builtin, agent_name, enabled, policy) "
        "SELECT json_extract(value,'$.name'), :name, json_extract(value,'$.description'), "
        "       json_extract(value,'$.parameters'), :store || '/' || json_extract(value,'$.handler'), "
        "       0, NULL, 1, json_extract(value,'$.policy') "
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

    /* attach to the owner (published stays 0 until publish). */
    rc |= run_ingest(db,
        "INSERT OR IGNORE INTO agent_extensions(agent_name, extension_name, enabled) "
        "VALUES(:owner, :name, 1)",
        manifest, name, store, owner_agent);

    /* channel (at most one): joined to the extension by name. */
    {
        char *ch = json_text(db, manifest, "'$.channel'");
        if (ch && ch[0]) {
            rc |= run_ingest(db,
                "INSERT OR REPLACE INTO channels(name, extension_name, type, binary_path) "
                "VALUES(:name, :name, json_extract(:m,'$.channel.type'), "
                "       :store || '/' || json_extract(:m,'$.channel.handler'))",
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
            while (sqlite3_step(st) == SQLITE_ROW) {
                const char *sname = (const char *)sqlite3_column_text(st, 0);
                const char *sched = (const char *)sqlite3_column_text(st, 1);
                const char *handler = (const char *)sqlite3_column_text(st, 2);
                if (!sname || !sched || !handler) continue;
                char task[PATH_MAX + 128];
                snprintf(task, sizeof(task),
                         "Run the scheduled extension script '%s': call js_eval with "
                         "filename '%s/%s'.", sname, store, handler);
                cron_add(db, owner_agent, sname, sched, 0, task);
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
