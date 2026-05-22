#include "db.h"
#include <stdio.h>
#include <assert.h>
#include <unistd.h>

static const char *DB_PATH = "/tmp/test_cclaw_cas.sqlite";

static void test_acquire_release(void) {
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "cas_test");
    assert(sid > 0);

    /* Acquire succeeds on idle session */
    assert(session_try_acquire(db, sid, "worker-1") == 0);

    /* Release succeeds with correct holder */
    assert(session_release(db, sid, "worker-1") == 0);

    db_close(db);
    printf("  PASS test_acquire_release\n");
}

static void test_double_acquire_rejected(void) {
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "cas_double");
    assert(sid > 0);

    assert(session_try_acquire(db, sid, "worker-1") == 0);
    /* Second acquire by different holder fails */
    assert(session_try_acquire(db, sid, "worker-2") == -1);
    /* Same holder also fails (already running) */
    assert(session_try_acquire(db, sid, "worker-1") == -1);

    assert(session_release(db, sid, "worker-1") == 0);
    db_close(db);
    printf("  PASS test_double_acquire_rejected\n");
}

static void test_release_wrong_holder(void) {
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "cas_wrong");
    assert(sid > 0);

    assert(session_try_acquire(db, sid, "worker-1") == 0);
    /* Release with wrong holder fails */
    assert(session_release(db, sid, "worker-2") == -1);
    /* Correct holder still works */
    assert(session_release(db, sid, "worker-1") == 0);

    db_close(db);
    printf("  PASS test_release_wrong_holder\n");
}

static void test_reacquire_after_release(void) {
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "cas_reacquire");
    assert(sid > 0);

    assert(session_try_acquire(db, sid, "w1") == 0);
    assert(session_release(db, sid, "w1") == 0);
    /* Different holder can now acquire */
    assert(session_try_acquire(db, sid, "w2") == 0);
    assert(session_release(db, sid, "w2") == 0);

    db_close(db);
    printf("  PASS test_reacquire_after_release\n");
}

int main(void) {
    unlink(DB_PATH);
    printf("test_session_cas:\n");
    test_acquire_release();
    test_double_acquire_rejected();
    test_release_wrong_holder();
    test_reacquire_after_release();
    unlink(DB_PATH);
    printf("All session CAS tests passed.\n");
    return 0;
}
