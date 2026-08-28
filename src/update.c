#define _GNU_SOURCE
#include "update.h"

#include "cclaw.h"
#include "config_registry.h"
#include "db.h"
#include "http.h"
#include "log.h"
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* How long to wait for the supervisor to bring the replacement daemon up
 * before calling the update failed. Generous: the target may be a 128MB ARMv5
 * box where process start is genuinely slow. */
#define RESTART_TIMEOUT_S 90
#define RESTART_POLL_MS   1000

/* Architecture → release asset. Compiled in rather than probed: the running
 * binary knows what it is, and guessing from uname would happily hand an
 * armv7 host an armv5 build. */
static const char *default_asset(void) {
#if defined(__x86_64__)
    return "cclaw-linux-x64";
#elif defined(__i386__)
    return "cclaw-linux-x86";
#elif defined(__arm__)
    return "cclaw-linux-armv5te";
#else
    return NULL;
#endif
}

/* ── tiny JSON field reader ────────────────────────────────────────
 * Only ever applied to the GitHub releases API, and only for "tag_name".
 * A dependency-free scan is the right size for one string field; anything
 * structural should use SQLite's JSON1 like the rest of the codebase. */
static char *json_string_field(const char *body, const char *field) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", field);
    const char *p = strstr(body, needle);
    if (!p) return NULL;
    p = strchr(p + strlen(needle), ':');
    if (!p) return NULL;
    while (*p && *p != '"') p++;
    if (!*p) return NULL;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return NULL;
    return strndup(p, (size_t)(end - p));
}

/* Run `path <flag>` and capture its first line. Returns malloc'd string or
 * NULL. This is how a *candidate* binary is interrogated before install — it
 * must never be given the database or any argument that could mutate state. */
static char *run_capture(const char *path, const char *flag) {
    int fds[2];
    if (pipe(fds) != 0) return NULL;

    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return NULL; }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        execl(path, path, flag, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);

    char buf[256] = "";
    ssize_t n = read(fds[0], buf, sizeof(buf) - 1);
    close(fds[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (n <= 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return NULL;

    buf[n] = '\0';
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    return strdup(buf);
}

/* json=1 for the releases API, 0 for a release asset — GitHub serves the two
 * from different hosts and honours Accept differently, so getting this wrong
 * silently yields the wrong body. */
static int http_get_to_memory(const char *url, HttpResponse *resp, int json) {
    const char *api_hdrs[]   = { "Accept: application/vnd.github+json",
                                 "X-GitHub-Api-Version: 2022-11-28", NULL };
    const char *asset_hdrs[] = { "Accept: application/octet-stream", NULL };
    const char **headers = json ? api_hdrs : asset_hdrs;
    HttpRequestOpts opts = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .timeout = 300,
        .follow_redirects = 1,
        .max_redirects = 5,
        .max_response_bytes = 64 * 1024 * 1024,
        .user_agent = "cclaw-update/1.0",
    };
    return http_do(&opts, resp);
}

/* Find the running daemon. The processes table is the daemon's own
 * registration, so this needs no pidfile and no guessing. */
/* instance_id, not pid, is a daemon's identity. It is a fresh random token per
 * registration, so it distinguishes a replacement from its predecessor even
 * when the pid is identical — which is exactly the case after a re-exec, where
 * the process keeps its pid and only the image changes. */
static pid_t running_daemon_pid(sqlite3 *db, int64_t *started_at,
                                char *instance_id, size_t id_cap) {
    if (instance_id && id_cap) instance_id[0] = '\0';
    sqlite3_stmt *st = NULL;
    pid_t pid = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT pid, started_at, instance_id FROM processes WHERE mode='daemon' "
            "ORDER BY heartbeat_at DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            pid = (pid_t)sqlite3_column_int(st, 0);
            if (started_at) *started_at = sqlite3_column_int64(st, 1);
            const char *id = (const char *)sqlite3_column_text(st, 2);
            if (instance_id && id_cap && id) snprintf(instance_id, id_cap, "%s", id);
        }
    }
    sqlite3_finalize(st);
    if (pid > 0 && kill(pid, 0) != 0 && errno == ESRCH) return 0;  /* stale row */
    return pid;
}

static int copy_file(const char *from, const char *to, mode_t mode) {
    FILE *in = fopen(from, "rb");
    if (!in) return -1;
    FILE *out = fopen(to, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[65536];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { rc = -1; break; }
    if (ferror(in)) rc = -1;
    fclose(in);
    if (fclose(out) != 0) rc = -1;
    if (rc == 0) chmod(to, mode);
    return rc;
}

/* readlink("/proc/self/exe") — the file we are about to replace. Not argv[0]:
 * that is whatever the caller typed, and "update" must act on the real image. */
static char *self_exe_path(void) {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return NULL;
    buf[n] = '\0';
    return strdup(buf);
}

/* Same discipline as the other verbs: open, refuse a foreign schema, and let
 * pending patches apply. The handshake below reads user_version *after* this,
 * so it compares a candidate against the shape the database will actually
 * have, not the one it had before this process touched it. */
static sqlite3 *update_db_open(void) {
    char *path = util_resolve_db_path();
    if (!path) { fprintf(stderr, "error: cannot resolve DB path\n"); return NULL; }
    sqlite3 *db = db_open(path);
    if (!db) { fprintf(stderr, "error: cannot open %s\n", path); free(path); return NULL; }
    if (!db_schema_compat(db)) {
        fprintf(stderr, "error: %s was created by a different cclaw schema\n", path);
        sqlite3_close(db); free(path); return NULL;
    }
    free(path);
    if (db_ensure_schema(db) != 0) {
        fprintf(stderr, "error: schema init failed\n");
        sqlite3_close(db); return NULL;
    }
    return db;
}

/* The handshake. A candidate that cannot read this database must never be
 * installed: patches are forward-only, so "install it and see" is a decision
 * that cannot be taken back by swapping the binary again. */
int update_schema_ok(const char *range, int db_version, char *why, size_t cap) {
    if (!range || !range[0]) {
        snprintf(why, cap, "candidate does not answer --schema-range "
                           "(older than this feature, or not a cclaw binary)");
        return 0;
    }
    int cand_min = 0, cand_cur = 0;
    if (sscanf(range, "min=%d current=%d", &cand_min, &cand_cur) != 2 ||
        cand_min <= 0 || cand_cur < cand_min) {
        snprintf(why, cap, "unparseable --schema-range output: %s", range);
        return 0;
    }
    if (db_version < cand_min) {
        snprintf(why, cap, "this database is schema v%d but the new build only "
                           "patches forward from v%d — it would refuse to open it",
                 db_version, cand_min);
        return 0;
    }
    if (db_version > cand_cur) {
        snprintf(why, cap, "this database is schema v%d and the new build only "
                           "knows up to v%d — that is a downgrade",
                 db_version, cand_cur);
        return 0;
    }
    return 1;
}

static int schema_compatible(const char *candidate, sqlite3 *db, char *why, size_t cap) {
    char *line = run_capture(candidate, "--schema-range");
    int uv = 0;
    db_schema_state(db, &uv);
    int ok = update_schema_ok(line, uv, why, cap);
    free(line);
    return ok;
}

/* Wait for the supervisor to bring a daemon up that is not the one we killed.
 * Identity is started_at from the daemon's own processes row, so this cannot
 * be fooled by a recycled pid. */
int update_await_restart(const char *db_path, const char *old_instance_id,
                         int timeout_s) {
    /* Deadline, not a count of sleeps. nanosleep() returns early when a signal
     * arrives, and this runs right after system() has reaped a shell — so
     * counting iterations quietly turns a 90s wait into a fraction of that,
     * and the caller reverts a restart that simply had not finished yet.
     * Resume the remaining interval on EINTR and judge by the clock. */
    time_t deadline = time(NULL) + timeout_s;
    do {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = RESTART_POLL_MS * 1000000L };
        while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
            ;   /* ts now holds the remainder */

        /* A fresh connection per poll, deliberately. Our own handle was opened
         * before the restart and can sit on a WAL read snapshot from then,
         * which makes the new daemon's row invisible no matter how long we
         * wait — the failure this loop is supposed to detect and the failure
         * it would report look identical from here. Reopening costs
         * microseconds once a second and removes the question. */
        sqlite3 *poll_db = NULL;
        if (sqlite3_open_v2(db_path, &poll_db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
            sqlite3_close(poll_db);
            continue;
        }
        char id[64] = "";
        pid_t pid = running_daemon_pid(poll_db, NULL, id, sizeof(id));
        sqlite3_close(poll_db);
        if (pid > 0 && id[0] && strcmp(id, old_instance_id) != 0) return 0;
    } while (time(NULL) < deadline);
    return -1;
}

static int update_install(sqlite3 *db, const char *self, const char *repo,
                          const char *asset, const char *tag) {
    char url[768], newpath[4096], prevpath[4096];
    snprintf(url, sizeof(url), "https://github.com/%s/releases/download/%s/%s",
             repo, tag, asset);
    snprintf(newpath, sizeof(newpath), "%s.new", self);
    snprintf(prevpath, sizeof(prevpath), "%s.prev", self);

    printf("downloading %s\n", url);
    HttpResponse r = {0};
    int status = http_get_to_memory(url, &r, 0);
    if (status != 200 || !r.data || r.len == 0) {
        fprintf(stderr, "error: download failed (HTTP %d%s%s)\n", status,
                r.err_detail[0] ? ": " : "", r.err_detail);
        http_response_free(&r);
        return 1;
    }
    if (r.truncated) {
        fprintf(stderr, "error: download was truncated at the size cap\n");
        http_response_free(&r);
        return 1;
    }

    FILE *f = fopen(newpath, "wb");
    if (!f || fwrite(r.data, 1, r.len, f) != r.len || fclose(f) != 0) {
        fprintf(stderr, "error: cannot write %s: %s\n", newpath, strerror(errno));
        if (f) fclose(f);
        http_response_free(&r);
        unlink(newpath);
        return 1;
    }
    size_t got_bytes = r.len;
    http_response_free(&r);
    chmod(newpath, 0755);

    /* Vet the candidate before it can touch anything. */
    char *ver = run_capture(newpath, "--version");
    if (!ver) {
        fprintf(stderr, "error: downloaded binary will not run here "
                        "(wrong architecture, or a missing shared library)\n");
        unlink(newpath);
        return 1;
    }
    printf("candidate: %s (%zu bytes)\n", ver, got_bytes);
    free(ver);

    char why[256] = "";
    if (!schema_compatible(newpath, db, why, sizeof(why))) {
        fprintf(stderr, "error: refusing %s — %s\n", tag, why);
        unlink(newpath);
        return 1;
    }

    /* Backstop for the failure the handshake cannot see: a build that migrates
     * the database fine and then dies for an unrelated reason. */
    const char *dbfile = sqlite3_db_filename(db, "main");
    /* Own the path: the revert below closes db, and sqlite3_db_filename's
     * pointer dies with the connection. */
    char dbpath_copy[4096] = "";
    if (dbfile) snprintf(dbpath_copy, sizeof(dbpath_copy), "%s", dbfile);
    char snap[4096] = "";
    if (dbfile && dbfile[0]) {
        snprintf(snap, sizeof(snap), "%s.preupdate", dbfile);
        unlink(snap);
        long long bytes = 0;
        if (db_backup_to(db, snap, &bytes) != 0) {
            fprintf(stderr, "error: could not snapshot the database — "
                            "not installing without a way back\n");
            unlink(newpath);
            return 1;
        }
        printf("database snapshot: %s (%lld bytes)\n", snap, bytes);
    }

    char old_instance[64] = "";
    pid_t pid = running_daemon_pid(db, NULL, old_instance, sizeof(old_instance));

    if (copy_file(self, prevpath, 0755) != 0) {
        fprintf(stderr, "error: cannot preserve the current binary\n");
        unlink(newpath);
        return 1;
    }
    /* Atomic: same directory, so a crash here leaves one whole binary or the
     * other, never a half-written one. */
    if (rename(newpath, self) != 0) {
        fprintf(stderr, "error: cannot install: %s\n", strerror(errno));
        unlink(newpath);
        return 1;
    }
    printf("installed %s (previous kept at %s)\n", tag, prevpath);

    if (pid <= 0) {
        printf("no daemon running — the new binary is in place\n");
        config_set(db, "update.installed_tag", tag);
        return 0;
    }

    /* Never stop a daemon without a known way to start it again.
     *
     * The obvious design — signal it and let the supervisor respawn — is
     * wrong, and testing on the Pogoplug is how that surfaced: a supervisor
     * worth the name treats a *graceful* exit as intentional and stays down
     * (the init script here is literally `[ $rc -eq 0 ] && break`). So a
     * SIGTERM leaves the box with no daemon and nothing to bring it back.
     *
     * With no restart command configured the safe move is to do nothing: the
     * running process holds its own inode, so it keeps serving the old code
     * quite happily until the operator restarts it, and the new binary is
     * already on disk waiting. */
    /* Two ways to get the running daemon onto the new code, and the default is
     * the portable one: SIGUSR2 tells it to shut down normally and then exec
     * the binary now on disk, keeping its pid. The supervisor never sees an
     * exit, so nothing here needs to know whether this box runs systemd, an
     * init script, or nothing at all — which is precisely the knowledge that
     * made the first version of this fragile.
     *
     * update.restart_command overrides it, for the case exec cannot cover:
     * exec inherits the current environment, so a deployment whose daemon
     * reads a changed env file at startup needs a real restart. */
    char *restart_cmd = config_get(db, "update.restart_command");
    if (restart_cmd && restart_cmd[0]) {
        printf("restarting daemon (pid %d): %s\n", (int)pid, restart_cmd);
        int cmd_rc = system(restart_cmd);
        if (cmd_rc != 0)
            fprintf(stderr, "warning: restart command exited %d — checking anyway\n",
                    cmd_rc);
    } else {
        printf("signalling daemon (pid %d) to restart into %s\n", (int)pid, tag);
        if (kill(pid, SIGUSR2) != 0) {
            fprintf(stderr, "error: could not signal the daemon: %s\n", strerror(errno));
            free(restart_cmd);
            config_set(db, "update.installed_tag", tag);
            printf("the new binary is installed; restart the daemon to apply it\n");
            return 0;
        }
    }
    free(restart_cmd);

    if (update_await_restart(dbpath_copy, old_instance, RESTART_TIMEOUT_S) == 0) {
        printf("daemon is back up on %s\n", tag);
        config_set(db, "update.installed_tag", tag);
        return 0;
    }

    fprintf(stderr, "error: daemon did not come back within %ds — reverting\n",
            RESTART_TIMEOUT_S);

    /* rename(), not a copy: this binary is *running*, and a running executable
     * cannot be written to (ETXTBSY) — but its directory entry can be
     * replaced. Install got this right and the revert did not, so the restore
     * failed at exactly the moment it existed for. */
    if (rename(prevpath, self) != 0)
        fprintf(stderr, "CRITICAL: could not restore %s from %s: %s — "
                        "do it by hand\n", self, prevpath, strerror(errno));
    else
        fprintf(stderr, "restored the previous binary\n");

    /* The database is deliberately NOT rolled back automatically. Writing over
     * a database file is an aggressive act with no safe way to know whether
     * something is attached to it, and the new daemon may have done real work
     * we would silently discard. The snapshot is right there, the schema
     * handshake already refused the migration hazard this would address, and a
     * stale database beats a corrupted one. Failure stays passive.  */
    if (snap[0])
        fprintf(stderr, "the database was left as it is; a pre-update snapshot "
                        "is at\n  %s\n", snap);
    fprintf(stderr, "reverted to the previous build — start the daemon "
                    "to confirm it is healthy\n");
    return 1;
}

/* ── periodic check ────────────────────────────────────────────────
 * Deliberately toothless: it looks, and if there is something new it tells the
 * agent. Installing stays an operator act, because an unattended self-update
 * is the one thing that can take a box off the network with nobody watching. */

static time_t g_next_check;   /* 0 = not scheduled yet */

/* Which agent hears about it: the routing default, which is the one a human is
 * actually talking to. */
static char *default_agent_name(sqlite3 *db) {
    char *name = config_get(db, "default_agent");
    if (name && name[0]) return name;
    free(name);
    sqlite3_stmt *st = NULL;
    char *out = NULL;
    if (sqlite3_prepare_v2(db, "SELECT name FROM agents ORDER BY created_at LIMIT 1",
                           -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        out = strdup((const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return out;
}

static int64_t recent_session(sqlite3 *db, const char *agent) {
    sqlite3_stmt *st = NULL;
    int64_t sid = 0;
    if (sqlite3_prepare_v2(db, "SELECT id FROM sessions WHERE agent_name=?"
                               " ORDER BY updated_at DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, agent, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) sid = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return sid;
}

void update_check_tick(sqlite3 *db) {
    int hours = config_get_int(db, "update.check_interval_hours");
    if (hours <= 0) return;

    time_t now = time(NULL);
    if (g_next_check == 0) {
        /* Not at startup: a restart loop would otherwise check every boot. */
        g_next_check = now + (time_t)hours * 3600;
        return;
    }
    if (now < g_next_check) return;
    g_next_check = now + (time_t)hours * 3600;

    char *repo = config_get(db, "update.repo");
    if (!repo || !repo[0]) { free(repo); return; }

    char url[512];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/latest", repo);
    HttpResponse r = {0};
    int status = http_get_to_memory(url, &r, 1);
    char *tag = (status == 200 && r.data) ? json_string_field(r.data, "tag_name") : NULL;
    http_response_free(&r);
    if (!tag) {
        /* A check that cannot reach GitHub is not an event worth waking an
         * agent for — it retries at the next interval. */
        LOG_DEBUG_("update check: no tag from %s (HTTP %d)", repo, status);
        free(repo);
        return;
    }

    char *installed = config_get(db, "update.installed_tag");
    char *notified = config_get(db, "update.notified_tag");
    int already = (installed && strcmp(installed, tag) == 0) ||
                  (notified && strcmp(notified, tag) == 0);
    free(installed);
    free(notified);
    if (already) { free(tag); free(repo); return; }

    char *agent = default_agent_name(db);
    int64_t sid = agent ? recent_session(db, agent) : 0;
    if (sid <= 0) {
        /* Nobody to tell yet. Leave notified_tag unset so the note is
         * delivered once a session exists, rather than being lost. */
        LOG_DEBUG_("update check: %s available, no session to notify yet", tag);
        free(agent); free(tag); free(repo);
        return;
    }

    char note[512];
    snprintf(note, sizeof(note),
             "A newer cclaw release is available: %s (this build is %s). "
             "Mention it to the operator — say what is new if you can find out, "
             "and that `cclaw update` installs it. Do not install it yourself.",
             tag, VERSION_COMMIT);
    if (inbox_insert(db, sid, "update", tag, note) > 0) {
        config_set(db, "update.notified_tag", tag);
        LOG_INFO_("update check: %s available, notified %s (session %lld)",
                  tag, agent, (long long)sid);
    }
    free(agent); free(tag); free(repo);
}

int update_main(int argc, char *argv[]) {
    int check_only = 0;
    const char *want_tag = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0) check_only = 1;
        else if (strcmp(argv[i], "--tag") == 0) {
            if (++i >= argc) { fprintf(stderr, "--tag requires a value\n"); return 2; }
            want_tag = argv[i];
        } else {
            fprintf(stderr, "usage: cclaw update [--check] [--tag vX.Y.Z]\n");
            return 2;
        }
    }

    char *self = self_exe_path();
    if (!self) { fprintf(stderr, "error: cannot resolve own path\n"); return 1; }

    sqlite3 *db = update_db_open();
    if (!db) { free(self); return 1; }

    char *repo = config_get(db, "update.repo");
    char *asset = config_get(db, "update.asset");
    char *installed = config_get(db, "update.installed_tag");
    if (!asset || !asset[0]) {
        free(asset);
        const char *d = default_asset();
        if (!d) {
            fprintf(stderr, "error: no release asset for this architecture — "
                            "set update.asset\n");
            free(self); free(repo); free(installed); sqlite3_close(db);
            return 1;
        }
        asset = strdup(d);
    }

    /* ── which tag ── */
    char *tag = NULL;
    if (want_tag) {
        tag = strdup(want_tag);
    } else {
        char url[512];
        snprintf(url, sizeof(url),
                 "https://api.github.com/repos/%s/releases/latest", repo);
        HttpResponse r = {0};
        int status = http_get_to_memory(url, &r, 1);
        if (status != 200 || !r.data) {
            fprintf(stderr, "error: cannot reach the releases API for %s (HTTP %d%s%s)\n",
                    repo, status, r.err_detail[0] ? ": " : "", r.err_detail);
            http_response_free(&r);
            free(self); free(repo); free(asset); free(installed); sqlite3_close(db);
            return 1;
        }
        tag = json_string_field(r.data, "tag_name");
        http_response_free(&r);
        if (!tag) {
            fprintf(stderr, "error: no tag_name in the releases API response\n");
            free(self); free(repo); free(asset); free(installed); sqlite3_close(db);
            return 1;
        }
    }

    printf("installed: %s\nlatest:    %s\n",
           installed && installed[0] ? installed : "(unknown)", tag);

    if (!want_tag && installed && strcmp(installed, tag) == 0) {
        printf("already up to date\n");
        free(self); free(repo); free(asset); free(installed); free(tag);
        sqlite3_close(db);
        return 0;
    }
    if (check_only) {
        printf("update available (not installed: --check)\n");
        free(self); free(repo); free(asset); free(installed); free(tag);
        sqlite3_close(db);
        return 10;
    }

    int rc = update_install(db, self, repo, asset, tag);

    free(self); free(repo); free(asset); free(installed); free(tag);
    sqlite3_close(db);
    return rc;
}
