#include "db.h"
#include "test_util.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>

static const char *DB_PATH = "/tmp/test_cclaw_inbox_rollback.sqlite";

/* rollback on missing session (leaf_id lookup fails) */
static void test_rollback_missing_session(void) {
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "rollback_miss", NULL, -1, 0);
    assert(sid > 0);

    inbox_insert(db, sid, "src", NULL, "msg1");
    inbox_insert(db, sid, "src", NULL, "msg2");

    /* Delete session so leaf_id lookup fails inside consume */
    sqlite3_exec(db, "DELETE FROM sessions WHERE id = 999999", NULL, NULL, NULL);
    /* Use a non-existent session_id */
    int consumed = inbox_consume_into_entries(db, 999999, 10);
    assert(consumed == -1);

    /* Original inbox items for real session still unconsumed */
    int count = 0;
    InboxItem *items = inbox_peek(db, sid, 10, &count);
    assert(count == 2);
    inbox_items_free(items, count);

    db_close(db);
    printf("  PASS test_rollback_missing_session\n");
}

/* Counter for progress handler abort */
static int g_abort_after;
static int g_op_count;

static int progress_abort(void *data) {
    (void)data;
    g_op_count++;
    if (g_op_count >= g_abort_after)
        return 1; /* non-zero = interrupt */
    return 0;
}

/* mid-transaction abort via progress handler → full rollback */
static void test_rollback_mid_consumption(void) {
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "rollback_mid", NULL, -1, 0);
    assert(sid > 0);

    /* Insert 5 inbox items */
    for (int i = 0; i < 5; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "item_%d", i);
        inbox_insert(db, sid, "src", NULL, buf);
    }

    /* Install progress handler that aborts after limited ops.
     * Try increasing abort thresholds until we find one that interrupts
     * mid-consumption (after at least 1 item processed but before commit). */
    int rollback_triggered = 0;
    for (int threshold = 10; threshold < 500; threshold += 5) {
        g_abort_after = threshold;
        g_op_count = 0;
        sqlite3_progress_handler(db, 1, progress_abort, NULL);

        int consumed = inbox_consume_into_entries(db, sid, 5);
        sqlite3_progress_handler(db, 0, NULL, NULL); /* remove handler */

        if (consumed == -1) {
            rollback_triggered = 1;
            break;
        }
        if (consumed == 5) {
            /* Succeeded before our threshold hit — undo for next attempt */
            /* Re-create scenario: reset consumed flags and delete entries */
            sqlite3_exec(db, "UPDATE inbox SET consumed=0 WHERE session_id=?", NULL, NULL, NULL);
            /* Simpler: just recreate */
            db_close(db);
            test_db_clean(DB_PATH);
            db = test_db_open(DB_PATH);
            sid = session_create(db, "rollback_mid", NULL, -1, 0);
            for (int i = 0; i < 5; i++) {
                char buf[32];
                snprintf(buf, sizeof(buf), "item_%d", i);
                inbox_insert(db, sid, "src", NULL, buf);
            }
        }
    }
    assert(rollback_triggered && "progress handler should have interrupted mid-transaction");

    /* After rollback: all inbox items still unconsumed */
    int count = 0;
    InboxItem *items = inbox_peek(db, sid, 10, &count);
    assert(count == 5);
    inbox_items_free(items, count);

    /* No entries created */
    int ecount = 0;
    Entry *entries = session_get_branch(db, sid, &ecount);
    assert(entries == NULL);
    assert(ecount == 0);

    db_close(db);
    printf("  PASS test_rollback_mid_consumption\n");
}

/* successful consume then verify no double-consume on retry */
static void test_no_double_consume_after_success(void) {
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "no_double", NULL, -1, 0);
    assert(sid > 0);

    inbox_insert(db, sid, "src", NULL, "once");

    int consumed = inbox_consume_into_entries(db, sid, 10);
    assert(consumed == 1);

    /* Second call should consume nothing */
    consumed = inbox_consume_into_entries(db, sid, 10);
    assert(consumed == 0);

    /* Only 1 entry exists */
    int ecount = 0;
    Entry *entries = session_get_branch(db, sid, &ecount);
    assert(ecount == 1);
    assert(strcmp(entries[0].message.content, "once") == 0);
    entry_branch_free(entries, ecount);

    db_close(db);
    printf("  PASS test_no_double_consume_after_success\n");
}

int main(void) {
    TEST_INIT();
    test_db_clean(DB_PATH);
    printf("test_inbox_rollback:\n");
    test_rollback_missing_session();
    test_rollback_mid_consumption();
    test_no_double_consume_after_success();
    test_db_clean(DB_PATH);
    printf("All inbox rollback tests passed.\n");
    return 0;
}
