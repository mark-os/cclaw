#include "db.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>

static const char *DB_PATH = "/tmp/test_cclaw_cron_inbox.sqlite";

/* T69/V16: Cron task goes through inbox + CAS lock pattern */
static void test_cron_inbox_insert_and_consume(void) {
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "cron_inbox");
    assert(sid > 0);

    /* Cron fires → insert task into inbox with source "cron" */
    int64_t id = inbox_insert(db, sid, "cron", "run daily report");
    assert(id > 0);

    /* Verify in inbox */
    int count = 0;
    InboxItem *items = inbox_peek(db, sid, 10, &count);
    assert(count == 1);
    assert(strcmp(items[0].source, "cron") == 0);
    assert(strcmp(items[0].payload, "run daily report") == 0);
    inbox_items_free(items, count);

    /* Acquire lock (simulating cron handler) */
    int rc = session_try_acquire(db, sid, "cron-123");
    assert(rc == 0);

    /* Consume inbox into entries */
    int consumed = inbox_consume_into_entries(db, sid, 100);
    assert(consumed == 1);

    /* Verify entry in session branch */
    int ecount = 0;
    Entry *entries = session_get_branch(db, sid, &ecount);
    assert(ecount == 1);
    assert(entries[0].message.role == ROLE_USER);
    assert(strcmp(entries[0].message.content, "run daily report") == 0);
    entry_branch_free(entries, ecount);

    session_release(db, sid, "cron-123");
    db_close(db);
    printf("  PASS test_cron_inbox_insert_and_consume\n");
}

/* V16: Cron skips execution if session already locked */
static void test_cron_inbox_lock_contention(void) {
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "cron_contention");
    assert(sid > 0);

    /* Another handler holds the lock */
    int rc = session_try_acquire(db, sid, "tg-active");
    assert(rc == 0);

    /* Cron fires → inserts to inbox */
    inbox_insert(db, sid, "cron", "scheduled task");

    /* Cron fails to acquire → message stays in inbox */
    rc = session_try_acquire(db, sid, "cron-456");
    assert(rc != 0);

    /* Inbox still has the message */
    int count = 0;
    InboxItem *items = inbox_peek(db, sid, 10, &count);
    assert(count == 1);
    assert(strcmp(items[0].payload, "scheduled task") == 0);
    inbox_items_free(items, count);

    session_release(db, sid, "tg-active");
    db_close(db);
    printf("  PASS test_cron_inbox_lock_contention\n");
}

/* Multiple cron tasks queued get consumed together when lock acquired */
static void test_cron_inbox_batch(void) {
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "cron_batch");
    assert(sid > 0);

    /* Multiple cron jobs fire while session was locked */
    inbox_insert(db, sid, "cron", "task1");
    inbox_insert(db, sid, "cron", "task2");

    /* Now acquire and consume all */
    int rc = session_try_acquire(db, sid, "cron-batch");
    assert(rc == 0);

    int consumed = inbox_consume_into_entries(db, sid, 100);
    assert(consumed == 2);

    int ecount = 0;
    Entry *entries = session_get_branch(db, sid, &ecount);
    assert(ecount == 2);
    assert(strcmp(entries[0].message.content, "task1") == 0);
    assert(strcmp(entries[1].message.content, "task2") == 0);
    entry_branch_free(entries, ecount);

    session_release(db, sid, "cron-batch");
    db_close(db);
    printf("  PASS test_cron_inbox_batch\n");
}

int main(void) {
    unlink(DB_PATH);
    printf("test_cron_inbox:\n");
    test_cron_inbox_insert_and_consume();
    test_cron_inbox_lock_contention();
    test_cron_inbox_batch();
    unlink(DB_PATH);
    printf("All cron inbox tests passed.\n");
    return 0;
}
