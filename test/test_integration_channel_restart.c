/* T252/T298: Test channel process crash → daemon restarts with backoff → max 3 → status='failed'
 * Uses a shell script as a fake channel binary that exits immediately.
 * T298: Uses pthread daemon (not fork) to avoid WAL visibility issues. */
#define _GNU_SOURCE
#include "daemon.h"
#include "db.h"
#include "config.h"
#include "shutdown.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

#define DB_PATH "/tmp/test_chan_restart.db"
#define FAKE_BIN "/tmp/test_chan_restart_bin.sh"

static void cleanup(void) {
    unlink(DB_PATH);
    char buf[256];
    snprintf(buf, sizeof(buf), "%s-wal", DB_PATH);
    unlink(buf);
    snprintf(buf, sizeof(buf), "%s-shm", DB_PATH);
    unlink(buf);
    snprintf(buf, sizeof(buf), "%s.pipe", DB_PATH);
    unlink(buf);
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

static void *daemon_thread(void *arg) {
    void **args = (void **)arg;
    Config *cfg = (Config *)args[0];
    sqlite3 *db = (sqlite3 *)args[1];
    daemon_run(cfg, db);
    return NULL;
}

static void test_channel_crash_restart(void) {
    cleanup();
    create_fake_binary();

    sqlite3 *db = db_open(DB_PATH);
    assert(db);

    /* Insert a channel that uses our fake binary */
    const char *sql =
        "INSERT INTO channels(name, type, binary_path, status)"
        " VALUES('crashy', 'custom', '" FAKE_BIN "', 'active');";
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    assert(rc == SQLITE_OK);

    Config cfg = {0};
    cfg.db_path = DB_PATH;

    shutdown_reset();

    void *args[2] = {&cfg, db};
    pthread_t dt;
    pthread_create(&dt, NULL, daemon_thread, args);

    /* Wait for daemon to exhaust restarts (initial + 3 restarts + mark failed) */
    int found = 0;
    for (int i = 0; i < 80; i++) {
        usleep(100000);
        sqlite3_stmt *stmt;
        rc = sqlite3_prepare_v2(db,
            "SELECT status FROM channels WHERE name='crashy';", -1, &stmt, NULL);
        if (rc == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
            const char *st = (const char *)sqlite3_column_text(stmt, 0);
            if (st && strcmp(st, "failed") == 0) found = 1;
        }
        sqlite3_finalize(stmt);
        if (found) break;
    }

    shutdown_request();
    pthread_join(dt, NULL);

    assert(found && "channel status should be 'failed' after max restarts");

    db_close(db);
    cleanup();
    printf("  PASS: test_channel_crash_restart\n");
}

int main(void) {
    printf("test_integration_channel_restart (T252/T298, V98):\n");
    test_channel_crash_restart();
    printf("All channel restart tests passed.\n");
    return 0;
}
