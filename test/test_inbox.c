#include "db.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>

static const char *DB_PATH = "/tmp/test_cclaw_inbox.sqlite";

static void test_inbox_insert(void) {
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "inbox_test");
    assert(sid > 0);

    int64_t id1 = inbox_insert(db, sid, "telegram", "{\"text\":\"hello\"}");
    assert(id1 > 0);
    int64_t id2 = inbox_insert(db, sid, "cron", "{\"job\":\"sweep\"}");
    assert(id2 > id1);

    db_close(db);
    printf("  PASS test_inbox_insert\n");
}

static void test_inbox_peek_empty(void) {
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "inbox_empty");
    assert(sid > 0);

    int count = -1;
    InboxItem *items = inbox_peek(db, sid, 10, &count);
    assert(items == NULL);
    assert(count == 0);

    db_close(db);
    printf("  PASS test_inbox_peek_empty\n");
}

static void test_inbox_peek_returns_unconsumed(void) {
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "inbox_peek");
    assert(sid > 0);

    inbox_insert(db, sid, "telegram", "msg1");
    inbox_insert(db, sid, "telegram", "msg2");
    inbox_insert(db, sid, "cron", "msg3");

    int count = 0;
    InboxItem *items = inbox_peek(db, sid, 10, &count);
    assert(items != NULL);
    assert(count == 3);
    /* Oldest first */
    assert(strcmp(items[0].payload, "msg1") == 0);
    assert(strcmp(items[1].payload, "msg2") == 0);
    assert(strcmp(items[2].source, "cron") == 0);

    inbox_items_free(items, count);
    db_close(db);
    printf("  PASS test_inbox_peek_returns_unconsumed\n");
}

static void test_inbox_peek_respects_limit(void) {
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "inbox_limit");
    assert(sid > 0);

    inbox_insert(db, sid, "src", "a");
    inbox_insert(db, sid, "src", "b");
    inbox_insert(db, sid, "src", "c");

    int count = 0;
    InboxItem *items = inbox_peek(db, sid, 2, &count);
    assert(items != NULL);
    assert(count == 2);
    assert(strcmp(items[0].payload, "a") == 0);
    assert(strcmp(items[1].payload, "b") == 0);

    inbox_items_free(items, count);
    db_close(db);
    printf("  PASS test_inbox_peek_respects_limit\n");
}

static void test_inbox_peek_session_isolation(void) {
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t s1 = session_create(db, "inbox_iso1");
    int64_t s2 = session_create(db, "inbox_iso2");

    inbox_insert(db, s1, "src", "for_s1");
    inbox_insert(db, s2, "src", "for_s2");

    int count = 0;
    InboxItem *items = inbox_peek(db, s1, 10, &count);
    assert(count == 1);
    assert(strcmp(items[0].payload, "for_s1") == 0);
    inbox_items_free(items, count);

    items = inbox_peek(db, s2, 10, &count);
    assert(count == 1);
    assert(strcmp(items[0].payload, "for_s2") == 0);
    inbox_items_free(items, count);

    db_close(db);
    printf("  PASS test_inbox_peek_session_isolation\n");
}

int main(void) {
    unlink(DB_PATH);
    printf("test_inbox:\n");
    test_inbox_insert();
    test_inbox_peek_empty();
    test_inbox_peek_returns_unconsumed();
    test_inbox_peek_respects_limit();
    test_inbox_peek_session_isolation();
    unlink(DB_PATH);
    printf("All inbox tests passed.\n");
    return 0;
}
