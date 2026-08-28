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
#include "update.h"
#include "test_util.h"

#define DB_PATH  "/tmp/test_daemon_lifecycle.db"
#define OUT_PATH "/tmp/test_daemon_lifecycle_daemon.txt"
#define HOME_DIR "/tmp/test_daemon_lifecycle_home"

#define FAIL(m) do { fprintf(stderr, "FAIL: %s\n", m); return 1; } while (0)

/* Bounded so a wedged daemon fails the assertion instead of the suite. */
#define START_TIMEOUT_S 20
#define NEG_TIMEOUT_S    3

static pid_t spawn_daemon(void) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int fd = open(OUT_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        setenv("CCLAW_DB_PATH", DB_PATH, 1);
        setenv("HOME", HOME_DIR, 1);
        setenv("OPENROUTER_API_KEY", "sk-test-not-used", 1);
        execl("build/cclaw", "cclaw", "--daemon", (char *)NULL);
        _exit(127);
    }
    return pid;
}

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
    int rc = update_await_restart(DB_PATH, first_started, first, NEG_TIMEOUT_S);
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
    rc = update_await_restart(DB_PATH, first_started, first, START_TIMEOUT_S);
    if (rc != 0) {
        stop_daemon(second);
        FAIL("a restarted daemon was not detected (the WAL-snapshot bug)");
    }
    int64_t second_started = 0;
    pid_t reg = read_registered(&second_started);
    if (reg != second) {
        stop_daemon(second);
        FAIL("processes does not name the running daemon");
    }
    printf("PASS (pid %d -> %d%s)\n", (int)first, (int)second,
           second_started == first_started ? ", same started_at second" : "");

    stop_daemon(second);

    /* ── and says no again once the daemon is gone ── */
    printf("  detects_daemon_gone... ");
    rc = update_await_restart(DB_PATH, second_started, second, NEG_TIMEOUT_S);
    if (rc == 0) FAIL("reported a restart with no daemon running");
    printf("PASS\n");

    unlink(DB_PATH);
    unlink(DB_PATH "-wal");
    unlink(DB_PATH "-shm");
    printf("4/4 passed\n");
    return 0;
}
