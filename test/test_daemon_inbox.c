#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define DB_PATH "/tmp/test_inbox_direct.db"

static int test_inbox_basic(void) {
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    if (!db) return 1;

    int64_t sid = session_create(db, "test", "agent", -1, 0);
    assert(sid > 0);

    int64_t id = inbox_insert(db, sid, "telegram", "hello");
    assert(id > 0);
    int count = inbox_count(db, sid);
    assert(count == 1);

    id = inbox_insert(db, sid, "cron", "task");
    assert(id > 0);
    count = inbox_count(db, sid);
    assert(count == 2);

    /* Different session — empty */
    count = inbox_count(db, 99);
    assert(count == 0);

    db_close(db);
    unlink(DB_PATH);
    printf("  PASS test_inbox_basic\n");
    return 0;
}

static int test_inbox_consume(void) {
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    if (!db) return 1;

    int64_t sid = session_create(db, "test", "agent", -1, 0);
    inbox_insert(db, sid, "sub-agent", "result data");

    int consumed = inbox_consume_into_entries(db, sid, 10);
    assert(consumed == 1);
    assert(inbox_count(db, sid) == 0);

    db_close(db);
    unlink(DB_PATH);
    printf("  PASS test_inbox_consume\n");
    return 0;
}

int main(void) {
    printf("test_inbox:\n");
    int rc = 0;
    rc |= test_inbox_basic();
    rc |= test_inbox_consume();
    if (rc == 0) printf("ALL PASSED\n");
    return rc;
}
