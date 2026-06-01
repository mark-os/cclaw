/* T252: Test channel process crash → daemon restarts with backoff → max 3 → status='failed'
 * Uses a shell script as a fake channel binary that exits immediately. */
#define _GNU_SOURCE
#include "daemon.h"
#include "db.h"
#include "config.h"
#include "secret.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define DB_PATH "/tmp/test_chan_restart.db"
#define FAKE_BIN "/tmp/test_chan_restart_bin.sh"

static void cleanup(void) {
    unlink(DB_PATH);
    unlink(DB_PATH "-wal");
    unlink(DB_PATH "-shm");
    unlink(DB_PATH ".key");
    unlink(DB_PATH ".pipe");
    unlink(FAKE_BIN);
}

/* Create a fake channel binary that exits immediately with code 1 */
static void create_fake_binary(void) {
    FILE *f = fopen(FAKE_BIN, "w");
    assert(f);
    fprintf(f, "#!/bin/sh\nexit 1\n");
    fclose(f);
    chmod(FAKE_BIN, 0755);
}

static void test_channel_crash_restart(void) {
    cleanup();
    create_fake_binary();

    sqlite3 *db = db_open_cclaw(DB_PATH);
    assert(db);

    /* Insert a channel that uses our fake binary */
    const char *sql =
        "INSERT INTO channels(name, type, binary_path, status)"
        " VALUES('crashy', 'custom', '" FAKE_BIN "', 'active');";
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    assert(rc == SQLITE_OK);

    /* Run daemon in a child process for a few seconds */
    Config cfg = {0};
    cfg.db_path = DB_PATH;

    /* Fork daemon, let it run briefly (immediate restarts, 3 crashes + mark failed < 2s) */
    pid_t dpid = fork();
    assert(dpid >= 0);
    if (dpid == 0) {
        /* Child: run daemon briefly */
        sqlite3 *cdb = db_open_cclaw(DB_PATH);
        daemon_run(&cfg, cdb);
        db_close(cdb);
        _exit(0);
    }

    db_close(db);

    /* Parent: wait for daemon to exhaust restarts */
    sleep(3);
    kill(dpid, SIGTERM);
    int status;
    waitpid(dpid, &status, 0);

    /* Verify channel status is 'failed' after 3 restart attempts */
    db = db_open_cclaw(DB_PATH);
    assert(db);
    sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_FULL, NULL, NULL);
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(db,
        "SELECT status FROM channels WHERE name='crashy';", -1, &stmt, NULL);
    assert(rc == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    const char *st = (const char *)sqlite3_column_text(stmt, 0);
    assert(st != NULL);
    assert(strcmp(st, "failed") == 0);
    sqlite3_finalize(stmt);

    db_close(db);
    cleanup();
    printf("  PASS: test_channel_crash_restart\n");
}

int main(void) {
    printf("test_integration_channel_restart (T252, V98):\n");
    test_channel_crash_restart();
    printf("All channel restart tests passed.\n");
    return 0;
}
