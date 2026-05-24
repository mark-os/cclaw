#include "db.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>

static const char *DB_PATH = "/tmp/test_cclaw_tg_intake.sqlite";

/* T68: Verify Telegram intake pattern: inbox_insert + CAS lock */
static void test_tg_intake_inserts_to_inbox(void) {
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "tg_intake", NULL);
    assert(sid > 0);

    /* Simulate Telegram message arriving → inbox_insert */
    int64_t id = inbox_insert(db, sid, "telegram", "hello from telegram");
    assert(id > 0);

    /* Verify it's in inbox */
    int count = 0;
    InboxItem *items = inbox_peek(db, sid, 10, &count);
    assert(count == 1);
    assert(strcmp(items[0].source, "telegram") == 0);
    assert(strcmp(items[0].payload, "hello from telegram") == 0);
    inbox_items_free(items, count);

    db_close(db);
    printf("  PASS test_tg_intake_inserts_to_inbox\n");
}

/* V16: If lock acquired, consume inbox into entries */
static void test_tg_intake_acquire_and_consume(void) {
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "tg_acquire", NULL);
    assert(sid > 0);

    /* Insert message to inbox */
    inbox_insert(db, sid, "telegram", "msg1");

    /* Acquire lock (simulating Telegram handler) */
    int rc = session_try_acquire(db, sid, "tg-123-456");
    assert(rc == 0);

    /* Consume inbox into entries */
    int consumed = inbox_consume_into_entries(db, sid, 100);
    assert(consumed == 1);

    /* Verify entry exists in session branch */
    int ecount = 0;
    Entry *entries = session_get_branch(db, sid, &ecount);
    assert(ecount == 1);
    assert(entries[0].message.role == ROLE_USER);
    assert(strcmp(entries[0].message.content, "msg1") == 0);
    entry_branch_free(entries, ecount);

    /* Release lock */
    session_release(db, sid, "tg-123-456");

    db_close(db);
    printf("  PASS test_tg_intake_acquire_and_consume\n");
}

/* V16: If lock not acquired, message stays in inbox */
static void test_tg_intake_lock_contention(void) {
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "tg_contention", NULL);
    assert(sid > 0);

    /* First handler acquires lock */
    int rc = session_try_acquire(db, sid, "tg-first");
    assert(rc == 0);

    /* Second message arrives while locked */
    inbox_insert(db, sid, "telegram", "second msg");

    /* Second handler fails to acquire */
    rc = session_try_acquire(db, sid, "tg-second");
    assert(rc != 0);

    /* Message remains in inbox */
    int count = 0;
    InboxItem *items = inbox_peek(db, sid, 10, &count);
    assert(count == 1);
    assert(strcmp(items[0].payload, "second msg") == 0);
    inbox_items_free(items, count);

    /* First handler releases */
    session_release(db, sid, "tg-first");

    db_close(db);
    printf("  PASS test_tg_intake_lock_contention\n");
}

/* Multiple messages queued in inbox get consumed together */
static void test_tg_intake_batch_consume(void) {
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "tg_batch", NULL);
    assert(sid > 0);

    /* Multiple messages arrive */
    inbox_insert(db, sid, "telegram", "first");
    inbox_insert(db, sid, "telegram", "second");
    inbox_insert(db, sid, "telegram", "third");

    /* Acquire and consume all */
    int rc = session_try_acquire(db, sid, "tg-batch");
    assert(rc == 0);

    int consumed = inbox_consume_into_entries(db, sid, 100);
    assert(consumed == 3);

    /* All in session branch, ordered */
    int ecount = 0;
    Entry *entries = session_get_branch(db, sid, &ecount);
    assert(ecount == 3);
    assert(strcmp(entries[0].message.content, "first") == 0);
    assert(strcmp(entries[2].message.content, "third") == 0);
    entry_branch_free(entries, ecount);

    session_release(db, sid, "tg-batch");
    db_close(db);
    printf("  PASS test_tg_intake_batch_consume\n");
}

int main(void) {
    unlink(DB_PATH);
    printf("test_telegram_intake:\n");
    test_tg_intake_inserts_to_inbox();
    test_tg_intake_acquire_and_consume();
    test_tg_intake_lock_contention();
    test_tg_intake_batch_consume();
    unlink(DB_PATH);
    printf("All telegram intake tests passed.\n");
    return 0;
}
