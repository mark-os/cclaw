#include "db.h"
#include "janitor.h"
#include <stdio.h>
#include <assert.h>
#include <unistd.h>

static const char *DB_PATH = "/tmp/test_cclaw_janitor.sqlite";

static void test_stale_lock_recovery(void) {
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "stale_test");
    assert(sid > 0);

    /* Acquire lock, then backdate lock_acquired_at to simulate staleness */
    assert(session_try_acquire(db, sid, "dead-worker") == 0);

    sqlite3_stmt *stmt;
    const char *sql = "UPDATE sessions SET lock_acquired_at = unixepoch() - 600 WHERE id = ?;";
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, sid);
    assert(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);

    /* Sweep with 300s timeout should recover it */
    int recovered = janitor_sweep(db, 300);
    assert(recovered == 1);

    /* Session should now be acquirable again */
    assert(session_try_acquire(db, sid, "new-worker") == 0);
    assert(session_release(db, sid, "new-worker") == 0);

    db_close(db);
    printf("  PASS test_stale_lock_recovery\n");
}

static void test_fresh_lock_not_recovered(void) {
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "fresh_test");
    assert(sid > 0);

    assert(session_try_acquire(db, sid, "active-worker") == 0);

    /* Sweep should NOT recover a fresh lock */
    int recovered = janitor_sweep(db, 300);
    assert(recovered == 0);

    /* Lock still held — second acquire fails */
    assert(session_try_acquire(db, sid, "other") == -1);

    assert(session_release(db, sid, "active-worker") == 0);
    db_close(db);
    printf("  PASS test_fresh_lock_not_recovered\n");
}

static void test_orphan_pending_recovery(void) {
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "pending_test");
    assert(sid > 0);

    /* Manually set state to 'pending' to simulate orphan */
    sqlite3_stmt *stmt;
    const char *sql = "UPDATE sessions SET state='pending' WHERE id = ?;";
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, sid);
    assert(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);

    int recovered = janitor_sweep(db, 300);
    assert(recovered == 1);

    /* Session should now be acquirable */
    assert(session_try_acquire(db, sid, "worker") == 0);
    assert(session_release(db, sid, "worker") == 0);

    db_close(db);
    printf("  PASS test_orphan_pending_recovery\n");
}

static void test_invalid_args(void) {
    sqlite3 *db = db_open(DB_PATH);
    assert(db);

    assert(janitor_sweep(NULL, 300) == -1);
    assert(janitor_sweep(db, 0) == -1);
    assert(janitor_sweep(db, -1) == -1);

    db_close(db);
    printf("  PASS test_invalid_args\n");
}

int main(void) {
    unlink(DB_PATH);
    printf("test_janitor:\n");
    test_stale_lock_recovery();
    test_fresh_lock_not_recovered();
    test_orphan_pending_recovery();
    test_invalid_args();
    unlink(DB_PATH);
    printf("All janitor tests passed.\n");
    return 0;
}
