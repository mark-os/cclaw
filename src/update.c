#define _GNU_SOURCE
#include "update.h"

#include "cclaw.h"
#include "config_registry.h"
#include "db.h"
#include "http.h"
#include "log.h"
#include "monocypher.h"
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
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
char *update_run_capture(const char *path, const char *flag) {
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

    /* Read to EOF, not once: loader warnings land on the pipe before the
     * binary's own line, and a single read can return just that first chunk
     * (exactly how the CI custom-libcurl target failed after the last-line
     * fix below went in). */
    char buf[512] = "";
    size_t n = 0;
    ssize_t r;
    while (n < sizeof(buf) - 1 &&
           ((r = read(fds[0], buf + n, sizeof(buf) - 1 - n)) > 0 ||
            (r < 0 && errno == EINTR)))
        if (r > 0) n += (size_t)r;
    close(fds[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (n == 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return NULL;

    buf[n] = '\0';
    /* The capture merges stderr, and a dynamic loader can print warnings
     * there before the binary says a word ("libcurl.so.4: no version
     * information available" broke the handshake on a real target). Keep the
     * LAST non-empty line — ours is the final thing the process prints —
     * rather than trusting the first. */
    char *last = NULL;
    for (char *p = strtok(buf, "\n"); p; p = strtok(NULL, "\n"))
        if (p[0]) last = p;
    return last ? strdup(last) : NULL;
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
    /* Anchor on the marker, not the line start — loader noise can precede it
     * even within one line on some libcs. */
    const char *m = strstr(range, "min=");
    if (!m || sscanf(m, "min=%d current=%d", &cand_min, &cand_cur) != 2 ||
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
    char *line = update_run_capture(candidate, "--schema-range");
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
                         int64_t since, int timeout_s) {
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
        int64_t started_at = 0;
        pid_t pid = running_daemon_pid(poll_db, &started_at, id, sizeof(id));
        sqlite3_close(poll_db);
        /* `since` matters after a rollback: the restored database carries a
         * processes row from before the snapshot whose instance_id is not the
         * one we signalled — and whose pid may even be alive again after the
         * re-exec. Only a row born after we asked counts. */
        if (pid > 0 && id[0] && strcmp(id, old_instance_id) != 0 &&
            (since <= 0 || started_at >= since)) return 0;
    } while (time(NULL) < deadline);
    return -1;
}

/* `update.last` is what --doctor shows for "what happened the last time
 * someone touched the binary": one JSON object, overwritten at every exit of
 * the install rails, by the crash-loop guard, and by `cclaw rollback`. */
static void update_last_set(sqlite3 *db, const char *tag, const char *outcome) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT json_object('tag', ?1, 'at', unixepoch(), 'outcome', ?2)",
            -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, tag, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, outcome, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW)
        config_set(db, "update.last", (const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
}

/* An update is the one operation that writes a whole database copy and a
 * second binary at once; on a small SD card it can be the write that fills
 * the disk. Same floor `cclaw backup` honours. What cannot be measured is
 * skipped, not refused. */
static int disk_room_ok(const char *dbfile, const char *self) {
    int floor_mb = config_default_int("disk_min_free_mb");
    if (floor_mb <= 0) return 1;
    struct stat st;
    long have;
    if (dbfile && dbfile[0] && stat(dbfile, &st) == 0 &&
        (have = util_free_mb(dbfile)) >= 0) {
        long need = (long)(st.st_size >> 20) + 1;
        if (have - need < floor_mb) {
            fprintf(stderr, "error: %ld MB free beside the database, the snapshot "
                            "needs ~%ld MB and the floor is %d MB "
                            "(disk_min_free_mb) — not installing\n",
                    have, need, floor_mb);
            return 0;
        }
    }
    if (stat(self, &st) == 0 && (have = util_free_mb(self)) >= 0) {
        long need = (long)(st.st_size >> 20) + 1;   /* .prev; .new is on disk already */
        if (have - need < floor_mb) {
            fprintf(stderr, "error: %ld MB free beside the binary, keeping the "
                            "previous build needs ~%ld MB and the floor is %d MB "
                            "(disk_min_free_mb) — not installing\n",
                    have, need, floor_mb);
            return 0;
        }
    }
    return 1;
}

/* Everything after "a candidate binary sits at <self>.new": vet, schema
 * handshake, DB snapshot, .prev, atomic swap, verify arm, restart, await,
 * revert. Shared between the GitHub download path and --file sideload —
 * the rails are the point of the verb, whichever way the bytes arrived. */
static int install_candidate(sqlite3 *db, const char *self, const char *tag) {
    char newpath[4096], prevpath[4096];
    snprintf(newpath, sizeof(newpath), "%s.new", self);
    snprintf(prevpath, sizeof(prevpath), "%s.prev", self);

    /* Vet the candidate before it can touch anything. */
    char *ver = update_run_capture(newpath, "--version");
    if (!ver) {
        fprintf(stderr, "error: candidate binary will not run here "
                        "(wrong architecture, or a missing shared library)\n");
        unlink(newpath);
        return 1;
    }
    printf("candidate: %s\n", ver);
    free(ver);

    char why[256] = "";
    if (!schema_compatible(newpath, db, why, sizeof(why))) {
        fprintf(stderr, "error: refusing %s — %s\n", tag, why);
        unlink(newpath);
        return 1;
    }

    const char *dbfile = sqlite3_db_filename(db, "main");
    if (!disk_room_ok(dbfile, self)) {
        unlink(newpath);
        return 1;
    }

    /* Backstop for the failure the handshake cannot see: a build that migrates
     * the database fine and then dies for an unrelated reason. */
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

    if (util_copy_file(self, prevpath, 0755) != 0) {
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
    /* Arm the crash-loop guard: the daemon counts its own starts against this
     * until it survives the window (update_verify_startup/_tick). */
    update_verify_arm(db, tag);

    if (pid <= 0) {
        printf("no daemon running — the new binary is in place\n");
        config_set(db, "update.installed_tag", tag);
        update_last_set(db, tag, "installed_no_daemon");
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
            update_last_set(db, tag, "installed_restart_pending");
            printf("the new binary is installed; restart the daemon to apply it\n");
            return 0;
        }
    }
    free(restart_cmd);

    if (update_await_restart(dbpath_copy, old_instance, 0, RESTART_TIMEOUT_S) == 0) {
        printf("daemon is back up on %s\n", tag);
        config_set(db, "update.installed_tag", tag);
        update_last_set(db, tag, "ok");
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
    /* The guard was armed for the build we just removed. */
    config_set(db, "update.verify", "");
    update_last_set(db, tag, "reverted_no_restart");

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
    if (snap[0])
        fprintf(stderr, "to discard what the new build wrote and go back to the "
                        "snapshot: cclaw rollback\n");
    return 1;
}

/* checksums.b2: one "<blake2b-512 hex>  <asset>" line per release asset, as
 * b2sum writes it in CI. BLAKE2b because monocypher is already in the binary
 * and has no SHA-256. Integrity, not authenticity: it catches a truncated or
 * wrong-arch download, not a hostile release page. */
int update_checksum_ok(const char *list, const char *asset,
                       const unsigned char *data, size_t len,
                       char *why, size_t cap) {
    uint8_t hash[64];
    crypto_blake2b(hash, sizeof(hash), data, len);
    char hex[129];
    for (int i = 0; i < 64; i++) snprintf(hex + 2 * i, 3, "%02x", hash[i]);

    size_t alen = strlen(asset);
    for (const char *line = list; line && *line; ) {
        const char *nl = strchr(line, '\n');
        const char *end = nl ? nl : line + strlen(line);
        /* b2sum separates with two spaces, or " *" in binary mode. */
        const char *name = line + 128;
        while (name < end && (*name == ' ' || *name == '*')) name++;
        if (end - line > 128 && (size_t)(end - name) == alen &&
            memcmp(name, asset, alen) == 0) {
            if (strncasecmp(line, hex, 128) == 0) return 1;
            snprintf(why, cap, "checksum mismatch for %s", asset);
            return 0;
        }
        line = nl ? nl + 1 : end;
    }
    snprintf(why, cap, "%s is not listed in the release's checksums.b2", asset);
    return 0;
}

static int update_install(sqlite3 *db, const char *self, const char *repo,
                          const char *asset, const char *tag) {
    char url[768], newpath[4096];
    snprintf(url, sizeof(url), "https://github.com/%s/releases/download/%s/%s",
             repo, tag, asset);
    snprintf(newpath, sizeof(newpath), "%s.new", self);

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

    /* Integrity against the release's checksums.b2, when it has one. Older
     * releases have none; that is reported, not refused. */
    snprintf(url, sizeof(url), "https://github.com/%s/releases/download/%s/checksums.b2",
             repo, tag);
    HttpResponse c = {0};
    int cstatus = http_get_to_memory(url, &c, 0);
    if (cstatus == 200 && c.data && c.len > 0) {
        char why[256] = "";
        if (!update_checksum_ok(c.data, asset, (const unsigned char *)r.data, r.len,
                                why, sizeof(why))) {
            fprintf(stderr, "error: %s — not installing\n", why);
            http_response_free(&c);
            http_response_free(&r);
            return 1;
        }
        printf("checksum verified (blake2b)\n");
    } else {
        printf("no checksums published for %s (HTTP %d) — skipping the integrity check\n",
               tag, cstatus);
    }
    http_response_free(&c);

    FILE *f = fopen(newpath, "wb");
    if (!f || fwrite(r.data, 1, r.len, f) != r.len || fclose(f) != 0) {
        fprintf(stderr, "error: cannot write %s: %s\n", newpath, strerror(errno));
        if (f) fclose(f);
        http_response_free(&r);
        unlink(newpath);
        return 1;
    }
    printf("downloaded %zu bytes\n", r.len);
    http_response_free(&r);
    chmod(newpath, 0755);

    return install_candidate(db, self, tag);
}

/* --file sideload: a binary the operator produced some other way (CI
 * artifact, cross build) goes through the exact same rails as a release
 * download. The label stamped into installed_tag / the verify marker is
 * derived from the filename, sanitized because it lands inside hand-built
 * JSON in update_verify_arm. */
static int update_install_file(sqlite3 *db, const char *self, const char *path) {
    char newpath[4096];
    snprintf(newpath, sizeof(newpath), "%s.new", self);

    if (util_copy_file(path, newpath, 0755) != 0) {
        fprintf(stderr, "error: cannot read %s: %s\n", path, strerror(errno));
        return 1;
    }

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char label[64];
    snprintf(label, sizeof(label), "file:%.57s", base);
    for (char *p = label; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || strchr(".-_:", *p)))
            *p = '_';

    printf("installing from %s\n", path);
    return install_candidate(db, self, label);
}

/* ── post-update crash-loop verification ───────────────────────────
 *
 * The 90s await above only proves the new build *started once*. A build that
 * starts, migrates, then keeps dying is invisible to it — the updater has
 * already exited, and the supervisor keeps respawning the broken build
 * forever. Same failure shape channel_swap already solved with flap
 * detection, so the same numbers: 3 starts inside a 5-minute window.
 *
 * Mechanism: update_install arms a marker (config `update.verify`); every
 * daemon startup while it is armed counts itself (update_verify_startup);
 * surviving the window clears it (update_verify_tick). Hitting the start
 * limit reverts the binary — after re-checking that the previous build can
 * still open this database, because schema patches are forward-only and a
 * revert that strands the DB is worse than the crash loop — then re-execs
 * into the restored build and tells the default agent what happened. A
 * healthy deployment pays one config read per boot for this. */

static char *default_agent_name(sqlite3 *db);
static int64_t recent_session(sqlite3 *db, const char *agent);

static void update_verify_notify(sqlite3 *db, const char *text) {
    char *agent = default_agent_name(db);
    int64_t sid = agent ? recent_session(db, agent) : 0;
    if (sid > 0) inbox_insert(db, sid, "update", "verify", text);
    LOG_ERROR_("%s", text);
    fprintf(stderr, "%s\n", text);
    free(agent);
}

void update_verify_arm(sqlite3 *db, const char *tag) {
    char v[160];
    /* tag is a release tag we just matched against GitHub — no escaping
     * hazard worth a JSON builder. */
    snprintf(v, sizeof(v), "{\"tag\":\"%.63s\",\"first_start\":0,\"starts\":0}", tag);
    config_set(db, "update.verify", v);
}

int update_verify_startup(sqlite3 *db) {
    char *v = config_get(db, "update.verify");
    if (!v || !v[0]) { free(v); return 0; }

    long long first = 0;
    int starts = 0;
    char tag[64] = "";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT COALESCE(json_extract(?1,'$.first_start'),0),"
            "       COALESCE(json_extract(?1,'$.starts'),0),"
            "       COALESCE(json_extract(?1,'$.tag'),'')"
            " WHERE json_valid(?1)", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, v, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            first = sqlite3_column_int64(st, 0);
            starts = sqlite3_column_int(st, 1);
            const char *t = (const char *)sqlite3_column_text(st, 2);
            if (t) snprintf(tag, sizeof(tag), "%s", t);
        }
    }
    sqlite3_finalize(st);
    free(v);

    time_t now = time(NULL);
    if (first == 0 || now - first > UPDATE_VERIFY_WINDOW) {
        /* First start under this marker, or the previous burst aged out —
         * open a fresh window. */
        first = now;
        starts = 1;
    } else {
        starts++;
    }

    if (starts < UPDATE_VERIFY_MAX_STARTS) {
        char nv[160];
        snprintf(nv, sizeof(nv),
                 "{\"tag\":\"%.63s\",\"first_start\":%lld,\"starts\":%d}",
                 tag, first, starts);
        config_set(db, "update.verify", nv);
        LOG_INFO_("update verify: start %d/%d for %s (window %llds)",
                  starts, UPDATE_VERIFY_MAX_STARTS, tag,
                  (long long)(now - first));
        return 0;
    }

    /* Crash loop. Decide whether the previous build is a legal target. */
    char self[4096];
    ssize_t sn = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (sn <= 0) {
        config_set(db, "update.verify", "");
        update_last_set(db, tag, "crash_loop_no_revert");
        update_verify_notify(db,
            "cclaw update verify: this build is crash-looping but its own "
            "path could not be resolved — no automatic revert; intervene by "
            "hand");
        return 0;
    }
    self[sn] = '\0';
    char prevpath[4096];
    snprintf(prevpath, sizeof(prevpath), "%.4085s.prev", self);

    char note[512];
    char why[256] = "";
    if (access(prevpath, X_OK) != 0) {
        config_set(db, "update.verify", "");
        update_last_set(db, tag, "crash_loop_no_revert");
        snprintf(note, sizeof(note),
                 "cclaw %.63s is crash-looping (%d starts in %d min) and no "
                 "previous binary is available at %.255s — no automatic "
                 "revert; intervene by hand.", tag, starts,
                 UPDATE_VERIFY_WINDOW / 60, prevpath);
        update_verify_notify(db, note);
        return 0;
    }
    if (!schema_compatible(prevpath, db, why, sizeof(why))) {
        /* Forward-only schema: a revert that cannot open the migrated DB
         * would brick the box harder than the crash loop does. Stay passive. */
        config_set(db, "update.verify", "");
        update_last_set(db, tag, "crash_loop_no_revert");
        snprintf(note, sizeof(note),
                 "cclaw %.63s is crash-looping (%d starts in %d min) but the "
                 "previous build cannot take this database back (%.200s) — "
                 "no automatic revert. `cclaw rollback` restores the pre-update "
                 "snapshot together with the previous build.", tag, starts,
                 UPDATE_VERIFY_WINDOW / 60, why);
        update_verify_notify(db, note);
        return 0;
    }

    /* Keep the bad build for diagnosis, then swap the directory entry —
     * rename, never a write, because this binary is running (ETXTBSY). */
    char badpath[4096];
    snprintf(badpath, sizeof(badpath), "%.4086s.bad", self);
    util_copy_file(self, badpath, 0755);   /* best-effort evidence */
    if (rename(prevpath, self) != 0) {
        config_set(db, "update.verify", "");
        update_last_set(db, tag, "crash_loop_no_revert");
        snprintf(note, sizeof(note),
                 "cclaw %.63s is crash-looping and the revert rename failed "
                 "(%.100s) — intervene by hand.", tag, strerror(errno));
        update_verify_notify(db, note);
        return 0;
    }
    config_set(db, "update.verify", "");
    update_last_set(db, tag, "crash_loop_reverted");
    snprintf(note, sizeof(note),
             "cclaw %.63s crash-looped (%d starts in %d min) and was "
             "automatically reverted to the previous build; the bad binary "
             "is kept at %.200s. Tell the operator.", tag, starts,
             UPDATE_VERIFY_WINDOW / 60, badpath);
    update_verify_notify(db, note);
    return 1;   /* caller re-execs into the restored build */
}

void update_verify_tick(sqlite3 *db) {
    static time_t proc_start;
    if (proc_start == 0) proc_start = time(NULL);
    if (time(NULL) - proc_start < UPDATE_VERIFY_WINDOW) return;
    char *v = config_get(db, "update.verify");
    int armed = v && v[0];
    free(v);
    if (!armed) return;
    config_set(db, "update.verify", "");
    char *tag = config_get(db, "update.installed_tag");
    update_last_set(db, tag && tag[0] ? tag : "", "verified");
    free(tag);
    LOG_INFO_("update verify: healthy for %ds — verified", UPDATE_VERIFY_WINDOW);
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
             "Mention it to the operator — say what is new if you can find "
             "out. If you have the install_update tool, offer to install it "
             "(that parks an approval — the operator decides); otherwise "
             "`cclaw update` installs it. Never install without approval.",
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
    const char *want_file = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0) check_only = 1;
        else if (strcmp(argv[i], "--tag") == 0) {
            if (++i >= argc) { fprintf(stderr, "--tag requires a value\n"); return 2; }
            want_tag = argv[i];
        } else if (strcmp(argv[i], "--file") == 0) {
            if (++i >= argc) { fprintf(stderr, "--file requires a path\n"); return 2; }
            want_file = argv[i];
        } else {
            fprintf(stderr,
                    "usage: cclaw update [--check] [--tag vX.Y.Z] [--file path]\n");
            return 2;
        }
    }
    if (want_file && (want_tag || check_only)) {
        fprintf(stderr, "--file does not combine with --tag or --check\n");
        return 2;
    }

    char *self = self_exe_path();
    if (!self) { fprintf(stderr, "error: cannot resolve own path\n"); return 1; }

    sqlite3 *db = update_db_open();
    if (!db) { free(self); return 1; }

    if (want_file) {
        int frc = update_install_file(db, self, want_file);
        free(self); sqlite3_close(db);
        return frc;
    }

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

/* ── rollback ──────────────────────────────────────────────────────
 *
 * The one thing the update rails deliberately never do on their own is put
 * the database back: swapping a file another process may hold open is how
 * databases get corrupted. `cclaw rollback` does it with consent and with the
 * file closed. The verb writes a marker — <db>.rollback, two lines: where the
 * current database goes, which binary to restore — and the swap itself
 * happens at one of three moments when nothing has the DB open: in the verb
 * when no daemon is running; in the daemon's shutdown tail; at daemon
 * startup before the DB is opened. Only a build that knows the protocol
 * applies it, and that is the build on disk — the *new* one — so a build too
 * old to have this verb can never half-apply it.
 *
 * Order: database first, binary second. The failure that leaves an old
 * binary beside a migrated database is the one that cannot start; the reverse
 * (old database, new binary) merely migrates again at the next start, with
 * the replaced copy kept beside it. */

static void rollback_marker_path(char *out, size_t cap, const char *db_path) {
    snprintf(out, cap, "%s.rollback", db_path);
}

static int rollback_marker_read(const char *db_path, char *failed, size_t fcap,
                                char *bin, size_t bcap) {
    char marker[4096];
    rollback_marker_path(marker, sizeof(marker), db_path);
    FILE *f = fopen(marker, "r");
    if (!f) return -1;
    int ok = fgets(failed, (int)fcap, f) && fgets(bin, (int)bcap, f);
    fclose(f);
    if (!ok) return -1;
    failed[strcspn(failed, "\n")] = '\0';
    bin[strcspn(bin, "\n")] = '\0';
    return failed[0] && bin[0] ? 0 : -1;
}

static int rollback_marker_write(const char *db_path, const char *failed,
                                 const char *bin) {
    char marker[4096];
    rollback_marker_path(marker, sizeof(marker), db_path);
    FILE *f = fopen(marker, "w");
    if (!f) return -1;
    int rc = fprintf(f, "%s\n%s\n", failed, bin) < 0 ? -1 : 0;
    if (fclose(f) != 0) rc = -1;
    if (rc != 0) unlink(marker);
    return rc;
}

/* WAL and shm travel with the file they belong to: a -wal left beside the
 * restored database would be replayed into it. Absent ones are fine. */
static void move_sidecars(const char *from, const char *to) {
    static const char *const sfx[] = { "-wal", "-shm" };
    for (int i = 0; i < 2; i++) {
        char a[4200], b[4200];
        snprintf(a, sizeof(a), "%s%s", from, sfx[i]);
        snprintf(b, sizeof(b), "%s%s", to, sfx[i]);
        (void)rename(a, b);
    }
}

int update_rollback_apply(const char *db_path, char *msg, size_t cap) {
    char failed[4096], bin[4096];
    if (rollback_marker_read(db_path, failed, sizeof(failed), bin, sizeof(bin)) != 0)
        return 0;
    char marker[4096], snap[4096], prev[4096], bad[4096];
    rollback_marker_path(marker, sizeof(marker), db_path);
    snprintf(snap, sizeof(snap), "%s.preupdate", db_path);
    snprintf(prev, sizeof(prev), "%.4090s.prev", bin);
    snprintf(bad, sizeof(bad), "%.4091s.bad", bin);

    /* Everything checked before anything moves: a rollback that stops halfway
     * is the one outcome worse than no rollback. The marker is consumed on
     * every path — a daemon must not re-attempt this at each start. */
    const char *missing = access(snap, R_OK) != 0 ? snap
                        : access(prev, X_OK) != 0 ? prev : NULL;
    if (missing) {
        snprintf(msg, cap, "rollback: %s is missing — nothing changed", missing);
        unlink(marker);
        return -1;
    }
    if (rename(db_path, failed) != 0) {
        snprintf(msg, cap, "rollback: cannot move %s aside: %s — nothing changed",
                 db_path, strerror(errno));
        unlink(marker);
        return -1;
    }
    move_sidecars(db_path, failed);
    if (rename(snap, db_path) != 0) {
        int e = errno;
        move_sidecars(failed, db_path);
        (void)rename(failed, db_path);
        snprintf(msg, cap, "rollback: cannot restore %s: %s — nothing changed",
                 snap, strerror(e));
        unlink(marker);
        return -1;
    }
    util_copy_file(bin, bad, 0755);   /* best-effort evidence, as the guard keeps */
    if (rename(prev, bin) != 0) {
        snprintf(msg, cap, "rollback: database restored from the snapshot but %s "
                 "could not replace %s (%s) — the build on disk will migrate it "
                 "again at the next start; restore the binary by hand",
                 prev, bin, strerror(errno));
        unlink(marker);
        return -1;
    }
    unlink(marker);
    snprintf(msg, cap, "rollback applied: database restored from the pre-update "
             "snapshot (the replaced one is kept at %s), binary restored from "
             "%s (the replaced one is kept at %s)", failed, prev, bad);
    return 1;
}

int rollback_main(int argc, char *argv[]) {
    int yes = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--yes") == 0) yes = 1;
        else { fprintf(stderr, "usage: cclaw rollback [--yes]\n"); return 2; }
    }

    int rc = 1;
    char *self = self_exe_path(), *prev_ver = NULL, *db_path = NULL;
    char *tag = NULL, *restart_cmd = NULL;
    sqlite3 *db = NULL;
    if (!self) { fprintf(stderr, "error: cannot resolve own path\n"); return 1; }

    char prev[4096], snap[4096], failed[4096], marker[4096];
    snprintf(prev, sizeof(prev), "%s.prev", self);
    if (access(prev, X_OK) != 0) {
        fprintf(stderr, "error: no previous build at %s — nothing to roll back to\n", prev);
        goto out;
    }
    prev_ver = update_run_capture(prev, "--version");
    if (!prev_ver) {
        fprintf(stderr, "error: the previous build at %s will not run\n", prev);
        goto out;
    }

    db_path = util_resolve_db_path();
    if (!db_path) { fprintf(stderr, "error: cannot resolve DB path\n"); goto out; }
    snprintf(snap, sizeof(snap), "%s.preupdate", db_path);
    rollback_marker_path(marker, sizeof(marker), db_path);
    struct stat sst;
    if (stat(snap, &sst) != 0) {
        fprintf(stderr, "error: no pre-update snapshot at %s — nothing to roll "
                        "back to (cclaw update takes one before it installs)\n", snap);
        goto out;
    }

    /* What the snapshot is — schema version and last entry — comes from the
     * file itself; no config key can drift from that. */
    int snap_version = 0;
    int64_t snap_max = 0;
    {
        sqlite3 *sdb = db_open_immutable(snap);
        if (!sdb) {
            fprintf(stderr, "error: cannot open the snapshot %s\n", snap);
            goto out;
        }
        db_schema_state(sdb, &snap_version);
        snap_max = db_scalar_i64(sdb, "SELECT COALESCE(MAX(id),0) FROM entries", 0, 0);
        sqlite3_close(sdb);
    }

    /* The same handshake update runs forward, run backward: the previous
     * build must be able to open the snapshot. */
    {
        char *range = update_run_capture(prev, "--schema-range");
        char why[256] = "";
        int ok = update_schema_ok(range, snap_version, why, sizeof(why));
        free(range);
        if (!ok) {
            fprintf(stderr, "error: the previous build cannot open the snapshot — %s\n", why);
            goto out;
        }
    }

    /* The live database is opened plainly and never migrated: this verb has to
     * work from the build whose migration is the thing being undone. */
    db = db_open(db_path);
    if (!db) { fprintf(stderr, "error: cannot open %s\n", db_path); goto out; }
    int64_t discard = db_scalar_i64(db, "SELECT COUNT(*) FROM entries WHERE id > ?",
                                    snap_max, 0);
    tag = config_get(db, "update.installed_tag");
    const char *label = tag && tag[0] ? tag : VERSION_COMMIT;
    snprintf(failed, sizeof(failed), "%s.failed-%s", db_path, label);
    if (access(failed, F_OK) == 0)
        snprintf(failed, sizeof(failed), "%s.failed-%s-%lld", db_path, label,
                 (long long)time(NULL));
    char old_instance[64] = "";
    pid_t pid = running_daemon_pid(db, NULL, old_instance, sizeof(old_instance));
    restart_cmd = config_get(db, "update.restart_command");

    /* The whole plan before the question. */
    char when[32];
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M", localtime(&sst.st_mtime));
    printf("rollback plan\n"
           "  database  restore %s\n"
           "            (schema v%d, taken %s)\n"
           "            the current database moves to %s\n"
           "            %lld entries written since the snapshot are discarded\n"
           "  binary    restore %s (%s)\n"
           "            the current build stays at %s.bad\n",
           snap, snap_version, when, failed, (long long)discard, prev, prev_ver, self);
    if (pid > 0)
        printf("  daemon    pid %d is running: it will be %s and the swap "
               "happens once it has closed the database\n", (int)pid,
               restart_cmd && restart_cmd[0] ? "restarted via update.restart_command"
                                             : "signalled to restart (SIGUSR2)");
    else
        printf("  daemon    not running: the swap happens now\n");

    if (!yes) {
        if (!isatty(0)) {
            fflush(stdout);
            fprintf(stderr, "not a terminal — pass --yes to proceed\n");
            rc = 2; goto out;
        }
        printf("proceed? [y/N] ");
        fflush(stdout);
        char ans[16] = "";
        if (!fgets(ans, sizeof(ans), stdin) || (ans[0] != 'y' && ans[0] != 'Y')) {
            printf("aborted\n");
            rc = 2; goto out;
        }
    }

    update_last_set(db, label, "rollback_requested");
    if (rollback_marker_write(db_path, failed, self) != 0) {
        fprintf(stderr, "error: cannot write %s: %s\n", marker, strerror(errno));
        goto out;
    }
    /* From here the marker drives everything, and this connection must be
     * gone before anything applies it. */
    sqlite3_close(db);
    db = NULL;

    char msg[8192];
    if (pid <= 0) {
        int arc = update_rollback_apply(db_path, msg, sizeof(msg));
        fprintf(arc == 1 ? stdout : stderr, "%s\n", msg);
        rc = arc == 1 ? 0 : 1;
        goto out;
    }

    int64_t since = time(NULL);
    if (restart_cmd && restart_cmd[0]) {
        printf("restarting daemon (pid %d): %s\n", (int)pid, restart_cmd);
        int cmd_rc = system(restart_cmd);
        if (cmd_rc != 0)
            fprintf(stderr, "warning: restart command exited %d — checking anyway\n",
                    cmd_rc);
    } else {
        printf("signalling daemon (pid %d) to restart\n", (int)pid);
        if (kill(pid, SIGUSR2) != 0) {
            fprintf(stderr, "error: could not signal the daemon: %s\nthe rollback "
                            "is pending in %s and applies when the daemon next "
                            "stops or starts\n", strerror(errno), marker);
            goto out;
        }
    }
    if (update_await_restart(db_path, old_instance, since, RESTART_TIMEOUT_S) == 0) {
        if (access(marker, F_OK) == 0) {
            fprintf(stderr, "error: the daemon is back but the rollback did not "
                            "apply (%s is still there) — check the daemon log\n",
                    marker);
            goto out;
        }
        printf("rolled back: daemon is back up on %s; the replaced database is "
               "kept at %s\n", prev_ver, failed);
        rc = 0;
        goto out;
    }
    fprintf(stderr, "daemon did not come back within %ds. The rollback stays "
                    "pending in\n  %s\nand applies the next time a daemon stops "
                    "or starts — or stop the daemon and run `cclaw rollback` "
                    "again.\n", RESTART_TIMEOUT_S, marker);

out:
    if (db) sqlite3_close(db);
    free(self); free(prev_ver); free(db_path); free(tag); free(restart_cmd);
    return rc;
}
