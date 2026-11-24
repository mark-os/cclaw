/* Process registry: heartbeat, liveness, gc, unregister */
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
    test_db_clean(DB_PATH);
}

static sqlite3 *fresh_db(void) {
    clean_db();
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db != NULL);
    return db;
}

static int proc_rows(sqlite3 *db, const char *id) {
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM processes WHERE instance_id=?;",
                              -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC);
    assert(sqlite3_step(st) == SQLITE_ROW);
    int n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
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
    assert(proc_rows(db, id) == 0);

    db_close(db);
    clean_db();
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
    assert(proc_rows(db, id) == 0);

    db_close(db);
    clean_db();
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
}

int main(void) {
    TEST_INIT();
    printf("test_processes:\n");
    RUN(test_stale_heartbeat_not_live);
    RUN(test_dead_pid_gc);
    RUN(test_unregister);
    return 0;
}
