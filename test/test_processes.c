/* Test process registry: register, heartbeat, liveness, gc, unregister */
#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define DB_PATH "/tmp/cclaw_test_processes.db"

static void clean_db(void) {
    unlink(DB_PATH);
    unlink(DB_PATH "-wal");
    unlink(DB_PATH "-shm");
}

static sqlite3 *fresh_db(void) {
    clean_db();
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db != NULL);
    return db;
}

static void test_register_writes_id(void) {
    sqlite3 *db = fresh_db();
    char id[64] = {0};
    assert(process_register(db, "cli", getpid(), id, sizeof(id)) == 0);
    assert(id[0] != '\0');
    assert(strlen(id) == 32); /* 16 random bytes = 32 hex chars */
    process_unregister(db, id);
    db_close(db);
    clean_db();
    printf("  PASS test_register_writes_id\n");
}

static void test_is_live_fresh(void) {
    sqlite3 *db = fresh_db();
    char id[64];
    assert(process_register(db, "cli", getpid(), id, sizeof(id)) == 0);
    assert(process_is_live(db, id, PROCESS_TTL_SEC) == 1);
    process_unregister(db, id);
    db_close(db);
    clean_db();
    printf("  PASS test_is_live_fresh\n");
}

static void test_stale_heartbeat_not_live(void) {
    sqlite3 *db = fresh_db();
    char id[64];
    assert(process_register(db, "cli", getpid(), id, sizeof(id)) == 0);

    /* Force heartbeat far in the past */
    char sql[256];
    snprintf(sql, sizeof(sql),
        "UPDATE processes SET heartbeat_at = unixepoch() - 9999 WHERE instance_id='%s';", id);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);

    assert(process_is_live(db, id, PROCESS_TTL_SEC) == 0);

    /* gc should remove it */
    assert(process_gc_dead(db, PROCESS_TTL_SEC) == 0);
    int64_t cnt = db_scalar_i64(db,
        "SELECT COUNT(*) FROM processes WHERE instance_id=?;",
        0, 0);
    /* db_scalar_i64 binds int64 on param 1 — use raw query instead */
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM processes WHERE instance_id=?;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_int(st, 0) == 0);
    sqlite3_finalize(st);
    (void)cnt;

    db_close(db);
    clean_db();
    printf("  PASS test_stale_heartbeat_not_live\n");
}

static void test_dead_pid_gc(void) {
    sqlite3 *db = fresh_db();

    /* Fork a child that exits immediately, then use its (reaped) pid */
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) _exit(0);
    int status;
    waitpid(child, &status, 0);

    /* Register with the now-dead pid */
    char id[64];
    assert(process_register(db, "cli", (int)child, id, sizeof(id)) == 0);

    /* Should not be live (pid gone) */
    assert(process_is_live(db, id, PROCESS_TTL_SEC) == 0);

    /* gc removes it */
    assert(process_gc_dead(db, PROCESS_TTL_SEC) == 0);
    sqlite3_stmt *st;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM processes WHERE instance_id=?;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_int(st, 0) == 0);
    sqlite3_finalize(st);

    db_close(db);
    clean_db();
    printf("  PASS test_dead_pid_gc\n");
}

static void test_unregister(void) {
    sqlite3 *db = fresh_db();
    char id[64];
    assert(process_register(db, "cli", getpid(), id, sizeof(id)) == 0);
    assert(process_is_live(db, id, PROCESS_TTL_SEC) == 1);
    assert(process_unregister(db, id) == 0);
    assert(process_is_live(db, id, PROCESS_TTL_SEC) == 0);
    db_close(db);
    clean_db();
    printf("  PASS test_unregister\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_processes:\n");
    test_register_writes_id();
    test_is_live_fresh();
    test_stale_heartbeat_not_live();
    test_dead_pid_gc();
    test_unregister();
    printf("all process registry tests passed\n");
    return 0;
}
