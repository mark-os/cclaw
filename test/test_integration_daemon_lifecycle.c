/* Integration test: daemon lifecycle as `cclaw update` observes it.
 *
 * This covers the gap that let three outages through on 2026-08-27. The dev
 * box runs no daemon, so nothing in the tree exercised supervisor interaction,
 * the `processes` table, or cross-process visibility — and the two bugs that
 * took the Pogoplug down were both invisible to reading:
 *
 *   1. Restart detection polled a connection opened *before* the restart. In
 *      WAL mode that connection can sit on a read snapshot from that moment,
 *      so the new daemon's row is never visible however long it waits — and
 *      "restarted fine" and "never came back" become indistinguishable, with
 *      an automatic rollback wired to the answer.
 *   2. started_at has one-second resolution, so a fast restart can report the
 *      same second the old daemon did.
 *
 * So the positive case here must use a *fast* restart (same second is the
 * interesting case, not the awkward one), and the negative case must prove the
 * detector can still say no — a detector that only ever returns success is
 * exactly as useless as one that only ever fails.
 *
 * Real forked `cclaw --daemon` processes against a temp DB. No network, no
 * channels, no API key needed for the daemon to register itself.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "db.h"
#include "config.h"
#include "config_registry.h"
#include "update.h"
#include "test_util.h"

#define DB_PATH  "/tmp/test_daemon_lifecycle.db"
#define OUT_PATH "/tmp/test_daemon_lifecycle_daemon.txt"
#define HOME_DIR "/tmp/test_daemon_lifecycle_home"
#define REEXEC_BIN "/tmp/test_daemon_lifecycle_cclaw"

#define FAIL(m) do { fprintf(stderr, "FAIL: %s\n", m); return 1; } while (0)

/* Bounded so a wedged daemon fails the assertion instead of the suite. */
#define START_TIMEOUT_S 20
#define NEG_TIMEOUT_S    3

static pid_t spawn_daemon_from(const char *bin) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int fd = open(OUT_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        setenv("CCLAW_DB_PATH", DB_PATH, 1);
        setenv("HOME", HOME_DIR, 1);
        setenv("OPENROUTER_API_KEY", "sk-test-not-used", 1);
        execl(bin, bin, "--daemon", (char *)NULL);
        _exit(127);
    }
    return pid;
}

static pid_t spawn_daemon(void) { return spawn_daemon_from("build/cclaw"); }

/* Read the registered daemon row through a *fresh* connection, the same way
 * the detector must. Returns 0 if no daemon is registered. */
static pid_t read_registered(int64_t *started_at) {
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(DB_PATH, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return 0;
    }
    sqlite3_stmt *st = NULL;
    pid_t pid = 0;
    if (sqlite3_prepare_v2(db, "SELECT pid, started_at FROM processes"
                               " WHERE mode='daemon' ORDER BY heartbeat_at DESC"
                               " LIMIT 1", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        pid = (pid_t)sqlite3_column_int(st, 0);
        if (started_at) *started_at = sqlite3_column_int64(st, 1);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return pid;
}

static void read_instance(char *out, size_t cap) {
    out[0] = '\0';
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(DB_PATH, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        sqlite3_close(db); return;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT instance_id FROM processes WHERE mode='daemon'"
                               " ORDER BY heartbeat_at DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) snprintf(out, cap, "%s", v);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
}

static pid_t wait_registered(pid_t want_pid, int64_t *started_at) {
    for (int i = 0; i < START_TIMEOUT_S * 10; i++) {
        int64_t s = 0;
        pid_t got = read_registered(&s);
        if (got > 0 && (want_pid == 0 || got == want_pid)) {
            if (started_at) *started_at = s;
            return got;
        }
        usleep(100000);
    }
    return 0;
}

static void stop_daemon(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    for (int i = 0; i < 100 && waitpid(pid, NULL, WNOHANG) == 0; i++)
        usleep(100000);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

int main(void) {
    TEST_INIT();
    printf("test_integration_daemon_lifecycle:\n");

    unlink(DB_PATH);
    unlink(DB_PATH "-wal");
    unlink(DB_PATH "-shm");
    unlink(OUT_PATH);
    mkdir(HOME_DIR, 0755);

    /* ── a daemon registers itself ── */
    printf("  daemon_registers... ");
    pid_t first = spawn_daemon();
    if (first < 0) FAIL("fork");
    int64_t first_started = 0;
    if (wait_registered(first, &first_started) != first) {
        stop_daemon(first);
        FAIL("daemon never registered in processes (see " OUT_PATH ")");
    }
    printf("PASS (pid %d)\n", (int)first);

    /* ── the detector says no while nothing has restarted ──
     * Runs first and deliberately: if this returned success the positive case
     * below would pass for free, and the whole test would be theatre. */
    printf("  detects_no_restart... ");
    time_t t0 = time(NULL);
    char first_instance[64] = "";
    read_instance(first_instance, sizeof(first_instance));
    if (!first_instance[0]) { stop_daemon(first); FAIL("no instance_id registered"); }
    int rc = update_await_restart(DB_PATH, first_instance, NEG_TIMEOUT_S);
    time_t waited = time(NULL) - t0;
    if (rc == 0) {
        stop_daemon(first);
        FAIL("reported a restart that never happened");
    }
    if (waited < NEG_TIMEOUT_S - 1) {
        stop_daemon(first);
        FAIL("gave up early — it was not really waiting");
    }
    printf("PASS (held for %llds)\n", (long long)waited);

    /* ── a real restart is detected ──
     * Immediately, so the replacement is likely to report the same started_at
     * second as the daemon it replaced. That is the case that regressed. */
    printf("  detects_restart... ");
    stop_daemon(first);
    pid_t second = spawn_daemon();
    if (second < 0) FAIL("fork");
    rc = update_await_restart(DB_PATH, first_instance, START_TIMEOUT_S);
    if (rc != 0) {
        stop_daemon(second);
        FAIL("a restarted daemon was not detected (the WAL-snapshot bug)");
    }
    int64_t second_started = 0;
    pid_t reg = read_registered(&second_started);
    char second_instance[64] = "";
    read_instance(second_instance, sizeof(second_instance));
    if (reg != second) {
        stop_daemon(second);
        FAIL("processes does not name the running daemon");
    }
    printf("PASS (pid %d -> %d%s)\n", (int)first, (int)second,
           second_started == first_started ? ", same started_at second" : "");

    stop_daemon(second);

    /* ── and says no again once the daemon is gone ── */
    printf("  detects_daemon_gone... ");
    rc = update_await_restart(DB_PATH, second_instance, NEG_TIMEOUT_S);
    if (rc == 0) FAIL("reported a restart with no daemon running");
    printf("PASS\n");

    /* ── SIGUSR2 re-exec: same pid, new image, new instance ──
     * The point of the feature: no supervisor involvement at all. This is also
     * the case pid-based identity cannot see, since exec keeps the pid — and
     * where exec'ing /proc/self/exe would silently relaunch the *old* inode
     * after a rename, so the test asserts the version actually changed. */
    printf("  reexec_adopts_new_binary... ");
    if (system("cp build/cclaw " REEXEC_BIN) != 0) FAIL("cp");
    pid_t third = spawn_daemon_from(REEXEC_BIN);
    if (third < 0) FAIL("fork");
    if (wait_registered(third, NULL) != third) {
        stop_daemon(third); FAIL("re-exec daemon never registered");
    }
    char third_instance[64] = "";
    read_instance(third_instance, sizeof(third_instance));

    /* Replace the binary underneath it, exactly as `cclaw update` does. */
    if (system("printf '#!/bin/sh\\nexit 9\\n' > " REEXEC_BIN ".new"
               " && chmod +x " REEXEC_BIN ".new"
               " && mv -f " REEXEC_BIN ".new " REEXEC_BIN) != 0)
        { stop_daemon(third); FAIL("could not replace the binary"); }

    kill(third, SIGUSR2);
    /* The replacement is a script that exits 9, so a successful exec means the
     * process is gone — proof the new file was executed, not the old inode. */
    int st = 0, gone = 0;
    for (int i = 0; i < 150; i++) {
        if (waitpid(third, &st, WNOHANG) == third) { gone = 1; break; }
        usleep(100000);
    }
    if (!gone) { stop_daemon(third); FAIL("daemon did not re-exec on SIGUSR2"); }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 9) {
        FAIL("re-exec ran something other than the replaced binary "
             "(exec'ing /proc/self/exe would relaunch the old inode)");
    }
    printf("PASS (pid %d kept, ran the replacement)\n", (int)third);
    unlink(REEXEC_BIN);

    /* ── crash-loop revert: 3 starts inside the window restore .prev ──
     * The failure update_await_restart cannot see: the build starts (so the
     * updater called it a success and exited), then keeps dying. Each start
     * below is a real daemon start counted by update_verify_startup; the
     * third must revert — rename .prev over the binary, keep the bad build
     * as .bad, clear the marker — and re-exec into the restored build. */
    printf("  crash_loop_reverts... ");
    if (system("cp build/cclaw " REEXEC_BIN
               " && cp build/cclaw " REEXEC_BIN ".prev") != 0) FAIL("cp");
    {
        sqlite3 *adb = NULL;
        if (sqlite3_open(DB_PATH, &adb) != SQLITE_OK) FAIL("arm open");
        update_verify_arm(adb, "vtest");
        sqlite3_close(adb);
    }
    pid_t loop_pid = 0;
    for (int start = 1; start <= 3; start++) {
        loop_pid = spawn_daemon_from(REEXEC_BIN);
        if (loop_pid < 0) FAIL("fork");
        if (wait_registered(loop_pid, NULL) != loop_pid) {
            stop_daemon(loop_pid);
            FAIL("loop daemon never registered");
        }
        if (start < 3) {
            /* Die the way a broken build dies — not a graceful exit. */
            kill(loop_pid, SIGKILL);
            waitpid(loop_pid, NULL, 0);
        }
    }
    /* Third start: wait for registration FIRST — on the loop path it happens
     * only after revert + re-exec (the guard runs before the daemon registers),
     * so a registered pid means the startup guard has already made its call.
     * Polling the files on a fixed short clock instead raced slow CI runners:
     * the daemon legitimately takes longer than 10s to reach the guard there. */
    if (wait_registered(loop_pid, NULL) != loop_pid) {
        stop_daemon(loop_pid);
        FAIL("reverted daemon did not come back (re-exec failed?)");
    }
    if (access(REEXEC_BIN ".prev", F_OK) == 0 ||
        access(REEXEC_BIN ".bad", F_OK) != 0) {
        stop_daemon(loop_pid);
        FAIL("third start did not revert (.prev still present or no .bad)");
    }
    {
        sqlite3 *vdb = NULL;
        char marker[8] = "x";
        if (sqlite3_open_v2(DB_PATH, &vdb, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
            sqlite3_stmt *ms = NULL;
            if (sqlite3_prepare_v2(vdb, "SELECT COALESCE(value,'') FROM config"
                                        " WHERE key='update.verify'",
                                   -1, &ms, NULL) == SQLITE_OK &&
                sqlite3_step(ms) == SQLITE_ROW)
                snprintf(marker, sizeof(marker), "%.7s",
                         (const char *)sqlite3_column_text(ms, 0));
            else
                marker[0] = '\0';   /* no row = cleared too */
            sqlite3_finalize(ms);
            sqlite3_close(vdb);
        }
        if (marker[0]) {
            stop_daemon(loop_pid);
            FAIL("marker not cleared after revert");
        }
    }
    stop_daemon(loop_pid);
    printf("PASS (reverted on start 3, daemon healthy after re-exec)\n");
    unlink(REEXEC_BIN);
    unlink(REEXEC_BIN ".bad");

    /* ── and stays passive when there is nothing to revert to ── */
    printf("  crash_loop_no_prev_stays_up... ");
    if (system("cp build/cclaw " REEXEC_BIN) != 0) FAIL("cp");
    {
        sqlite3 *adb = NULL;
        if (sqlite3_open(DB_PATH, &adb) != SQLITE_OK) FAIL("arm open");
        /* Pre-aged marker: this start is the third. */
        char v[128];
        snprintf(v, sizeof(v),
                 "{\"tag\":\"vtest2\",\"first_start\":%lld,\"starts\":2}",
                 (long long)time(NULL));
        sqlite3_stmt *st = NULL;
        sqlite3_prepare_v2(adb, "INSERT OR REPLACE INTO config(key,value)"
                                " VALUES('update.verify',?1)", -1, &st, NULL);
        sqlite3_bind_text(st, 1, v, -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
        sqlite3_close(adb);
    }
    pid_t solo = spawn_daemon_from(REEXEC_BIN);
    if (solo < 0) FAIL("fork");
    if (wait_registered(solo, NULL) != solo) {
        stop_daemon(solo);
        FAIL("daemon with no .prev did not stay up (revert should be passive)");
    }
    stop_daemon(solo);
    printf("PASS (no .prev: notified, kept running)\n");
    unlink(REEXEC_BIN);

    unlink(DB_PATH);
    unlink(DB_PATH "-wal");
    unlink(DB_PATH "-shm");
    printf("7/7 passed\n");
    return 0;
}
