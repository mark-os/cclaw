/* Deaf-channel watchdog: a runner that is alive as a process but whose
 * transport has stopped delivering is bounced through the ordinary supervised
 * restart path (SIGTERM → reap → backoff → respawn).
 *
 * The "runner" here is this very binary re-exec'd: channel supervision forks
 * /proc/self/exe with `--channel <name>`, so a stub branch at the top of main
 * turns the test binary into a process that is alive and permanently silent —
 * exactly the failure under test — with no network, no JS engine, and no mock
 * server. Everything else (fork/exec, the kill, the reap, the backoff
 * respawn) is the real supervision code.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE          /* usleep — dropped from POSIX 2008 */
#include "channel.h"
#include "approval.h"
#include "resolve.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* channel.o references resolve_approval — stub keeps resolve.o out of the
 * link, same as test_channel_lifecycle.c. */
void resolve_approval(int64_t approval_id, ApprovalDecision decision,
                      const char *decided_via, int64_t grant_expires_at) {
    (void)approval_id; (void)decision; (void)decided_via; (void)grant_expires_at;
}

#define TEST_DB "/tmp/test_channel_deaf.sqlite"
#define CH "wedged"

/* Every derived artifact, not just the .db — a killed run leaves -wal/-shm and
 * the per-channel FIFO/socket behind, and stale debris makes the next run hang
 * somewhere new (AGENTS.md Testing Guidelines). */
static void clean_all(void) {
    test_db_clean(TEST_DB);
    unlink(TEST_DB "." CH ".pipe");
    unlink(TEST_DB "." CH ".sock");
}

/* An active channel that passes the launch gate. deaf_timeout NULL = leave the
 * per-channel key unset. */
static sqlite3 *seed(const char *deaf_timeout) {
    clean_all();
    sqlite3 *db = test_db_open(TEST_DB);
    assert(db);
    test_seed_agent(db, "default");
    assert(sqlite3_exec(db,
        "INSERT INTO extensions(name, path, version, owner_agent, published, enabled)"
        " VALUES('" CH "', '/tmp/test_channel_deaf_ext', '0.0.0', 'default', 0, 1);",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_exec(db,
        "INSERT INTO channels(name, extension_name, type, binary_path, status)"
        " VALUES('" CH "', '" CH "', 'stub', '', 'active');",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_exec(db,
        "INSERT INTO config(key, value, default_value, description)"
        " VALUES('" CH ".enabled', '1', '0', 'launch gate');",
        NULL, NULL, NULL) == SQLITE_OK);
    if (deaf_timeout) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO config(key, value, default_value, description)"
                 " VALUES('" CH ".deaf_timeout', '%s', '', 'watchdog');",
                 deaf_timeout);
        assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    }
    return db;
}

static int64_t chan_pid(sqlite3 *db) {
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "SELECT COALESCE(pid,0) FROM channels WHERE name='" CH "';",
        -1, &s, NULL) == SQLITE_OK);
    int64_t p = 0;
    if (sqlite3_step(s) == SQLITE_ROW) p = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return p;
}

static void backdate(sqlite3 *db, int seconds) {
    char sql[256];
    snprintf(sql, sizeof(sql),
             "UPDATE channel_state SET value = unixepoch() - %d"
             " WHERE channel_name='" CH "' AND key='last_activity_at';", seconds);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_changes(db) == 1);
}

/* kill(pid,0) also succeeds for a zombie, so ask waitpid instead: 0 means the
 * child is genuinely still running. */
static int still_running(pid_t pid) {
    int st;
    return waitpid(pid, &st, WNOHANG) == 0;
}

/* Bounded wait for a signalled child to go. The SIGTERM lands in microseconds;
 * the loop only bounds a failure so the suite reports FAIL, not TIMEOUT. */
static int wait_gone(pid_t pid) {
    for (int i = 0; i < 50; i++) {
        int st;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r != 0) return 1;              /* reaped, or already gone */
        usleep(100000);
    }
    return 0;
}

/* ── Threshold resolution: <ext>.deaf_timeout > channel_deaf_timeout > built-in ── */
static void test_threshold_resolution(void) {
    sqlite3 *db = seed(NULL);
    unsetenv("CCLAW_CHANNEL_DEAF_TIMEOUT");

    assert(channel_deaf_timeout(db, CH) == CHANNEL_DEAF_TIMEOUT);

    assert(sqlite3_exec(db,
        "INSERT INTO config(key, value, default_value, description)"
        " VALUES('channel_deaf_timeout', '120', '', 'watchdog');",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(channel_deaf_timeout(db, CH) == 120);

    /* Per-channel wins over the global. */
    assert(sqlite3_exec(db,
        "INSERT INTO config(key, value, default_value, description)"
        " VALUES('" CH ".deaf_timeout', '45', '', 'watchdog');",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(channel_deaf_timeout(db, CH) == 45);

    /* A per-channel 0 is an answer ("off here"), not an absence. */
    assert(sqlite3_exec(db, "UPDATE config SET value='0'"
        " WHERE key='" CH ".deaf_timeout';", NULL, NULL, NULL) == SQLITE_OK);
    assert(channel_deaf_timeout(db, CH) == 0);

    /* Garbage is not an answer — fall through to the next tier. */
    assert(sqlite3_exec(db, "UPDATE config SET value='soon'"
        " WHERE key='" CH ".deaf_timeout';", NULL, NULL, NULL) == SQLITE_OK);
    assert(channel_deaf_timeout(db, CH) == 120);

    /* Env beats the DB — the uniform config resolution rule. */
    setenv("CCLAW_CHANNEL_DEAF_TIMEOUT", "7", 1);
    assert(channel_deaf_timeout(db, CH) == 7);
    unsetenv("CCLAW_CHANNEL_DEAF_TIMEOUT");

    db_close(db);
    printf("  PASS test_threshold_resolution\n");
}

/* ── Enrolment: only received traffic creates the mark ── */
static void test_enrolment(void) {
    sqlite3 *db = seed(NULL);

    /* Never received: no mark, and the watchdog has no opinion. */
    assert(channel_activity_age(db, CH) == -1);

    /* reset never enrols — a webhook channel (fed by the daemon over the
     * request UDS) has nothing that could wedge and must stay unwatched. */
    channel_activity_reset(db, CH);
    assert(channel_activity_age(db, CH) == -1);

    channel_activity_seen(db, CH);
    assert(channel_activity_age(db, CH) <= 2);

    backdate(db, 900);
    assert(channel_activity_age(db, CH) >= 900);

    /* Now that a mark exists, reset refreshes it. */
    channel_activity_reset(db, CH);
    assert(channel_activity_age(db, CH) <= 2);

    db_close(db);
    printf("  PASS test_enrolment\n");
}

/* ── The failure this exists for ── */
static void test_wedged_runner_restarts(void) {
    sqlite3 *db = seed("60");
    channel_activity_seen(db, CH);        /* it has received traffic before */

    channel_tick(db);                     /* reconcile → fork+exec the stub */
    pid_t pid = (pid_t)chan_pid(db);
    assert(pid > 0);
    assert(still_running(pid));

    /* Launch restarts the clock: a freshly forked runner is never instantly
     * deaf, however stale the mark it inherited from its predecessor. */
    assert(channel_activity_age(db, CH) <= 2);
    channel_tick(db);
    assert(still_running(pid));

    /* Now it goes deaf: process up, transport silent. */
    backdate(db, 3600);
    channel_tick(db);
    assert(wait_gone(pid));

    /* The bounce reset the mark, so a child that ignored SIGTERM would be
     * re-signalled once per threshold rather than once per tick. */
    assert(channel_activity_age(db, CH) <= 2);

    /* …and it lands in the ordinary supervised-restart path. */
    assert(channel_reap(pid, db) == 1);
    assert(channel_next_deadline() > 0);

    pid_t next = 0;
    for (int i = 0; i < 60 && !next; i++) {
        channel_tick(db);
        pid_t p = (pid_t)chan_pid(db);
        if (p > 0 && p != pid) next = p;
        else usleep(100000);
    }
    assert(next > 0);
    assert(still_running(next));

    channel_shutdown_all();
    db_close(db);
    printf("  PASS test_wedged_runner_restarts\n");
}

/* ── A channel with no watched transport is never judged deaf ── */
static void test_unenrolled_never_bounced(void) {
    sqlite3 *db = seed("60");

    channel_tick(db);
    pid_t pid = (pid_t)chan_pid(db);
    assert(pid > 0);
    assert(channel_activity_age(db, CH) == -1);   /* launch did not enrol it */

    channel_tick(db);
    assert(still_running(pid));

    channel_shutdown_all();
    db_close(db);
    printf("  PASS test_unenrolled_never_bounced\n");
}

/* ── deaf_timeout=0 is the off switch ── */
static void test_watchdog_disabled(void) {
    sqlite3 *db = seed("0");
    channel_activity_seen(db, CH);

    channel_tick(db);
    pid_t pid = (pid_t)chan_pid(db);
    assert(pid > 0);

    backdate(db, 3600);
    channel_tick(db);
    assert(still_running(pid));
    assert(channel_activity_age(db, CH) >= 3600);   /* mark left alone */

    channel_shutdown_all();
    db_close(db);
    printf("  PASS test_watchdog_disabled\n");
}

int main(int argc, char **argv) {
    /* Re-exec'd by channel supervision as `cclaw --channel <name>`: be the
     * wedged runner — alive, and permanently silent. Must come before
     * anything that touches the test DB or stdout. */
    if (argc >= 3 && strcmp(argv[1], "--channel") == 0) {
        for (;;) pause();
    }

    TEST_INIT();
    /* One intentional wait (the 2s respawn backoff). Well below the Makefile's
     * 20s wrapper, so a real hang still reports FAIL rather than TIMEOUT. */
    alarm(15);
    printf("test_channel_deaf:\n");
    test_threshold_resolution();
    test_enrolment();
    test_wedged_runner_restarts();
    test_unenrolled_never_bounced();
    test_watchdog_disabled();
    clean_all();
    printf("all channel deaf-watchdog tests passed\n");
    return 0;
}
