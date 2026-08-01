#include "db.h"
#include "test_util.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>

static const char *DB_PATH = "/tmp/test_cclaw_inbox.sqlite";

static void test_inbox_insert(void) {
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "inbox_test", NULL, -1, 0);
    assert(sid > 0);

    int64_t id1 = inbox_insert(db, sid, "telegram", NULL, "{\"text\":\"hello\"}");
    assert(id1 > 0);
    int64_t id2 = inbox_insert(db, sid, "cron", NULL, "{\"job\":\"sweep\"}");
    assert(id2 > id1);

    db_close(db);
    printf("  PASS test_inbox_insert\n");
}

static void test_inbox_peek_empty(void) {
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "inbox_empty", NULL, -1, 0);
    assert(sid > 0);

    int count = -1;
    InboxItem *items = inbox_peek(db, sid, 10, &count);
    assert(items == NULL);
    assert(count == 0);

    db_close(db);
    printf("  PASS test_inbox_peek_empty\n");
}

static void test_inbox_peek_returns_unconsumed(void) {
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "inbox_peek", NULL, -1, 0);
    assert(sid > 0);

    inbox_insert(db, sid, "telegram", NULL, "msg1");
    inbox_insert(db, sid, "telegram", NULL, "msg2");
    inbox_insert(db, sid, "cron", NULL, "msg3");

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
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "inbox_limit", NULL, -1, 0);
    assert(sid > 0);

    inbox_insert(db, sid, "src", NULL, "a");
    inbox_insert(db, sid, "src", NULL, "b");
    inbox_insert(db, sid, "src", NULL, "c");

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
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    int64_t s1 = session_create(db, "inbox_iso1", NULL, -1, 0);
    int64_t s2 = session_create(db, "inbox_iso2", NULL, -1, 0);

    inbox_insert(db, s1, "src", NULL, "for_s1");
    inbox_insert(db, s2, "src", NULL, "for_s2");

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

static void test_inbox_consume_into_entries(void) {
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "inbox_consume", NULL, -1, 0);
    assert(sid > 0);

    /* Insert 3 inbox items */
    inbox_insert(db, sid, "telegram", NULL, "hello");
    inbox_insert(db, sid, "telegram", NULL, "world");
    inbox_insert(db, sid, "cron", NULL, "tick");

    /* Consume all */
    int consumed = inbox_consume_into_entries(db, sid, 10);
    assert(consumed == 3);

    /* Peek should return nothing (all consumed) */
    int count = 0;
    InboxItem *items = inbox_peek(db, sid, 10, &count);
    assert(items == NULL);
    assert(count == 0);

    /* Entries should exist in session branch */
    int ecount = 0;
    Entry *entries = session_get_branch(db, sid, &ecount);
    assert(entries != NULL);
    assert(ecount == 3);
    assert(entries[0].message.role == ROLE_USER);
    assert(strcmp(entries[0].message.content, "hello") == 0);
    assert(strcmp(entries[1].message.content, "world") == 0);
    assert(strcmp(entries[2].message.content, "tick") == 0);
    /* Verify parent chain */
    assert(entries[0].parent_id == -1);
    assert(entries[1].parent_id == entries[0].id);
    assert(entries[2].parent_id == entries[1].id);
    entry_branch_free(entries, ecount);

    db_close(db);
    printf("  PASS test_inbox_consume_into_entries\n");
}

/* source_ref becomes a provenance tag on the entry at drain time — the model
 * must see which job/child spoke without a join the referent may not survive.
 * A NULL source_ref (every writer but agent_result today) drains verbatim. */
static void test_inbox_consume_tags_source_ref(void) {
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "inbox_source_ref", NULL, -1, 0);
    assert(sid > 0);

    assert(inbox_insert(db, sid, "cron", "jobname", "sweep the logs") > 0);
    assert(inbox_insert(db, sid, "agent_result", "77", "Sub-agent completed: ok") > 0);
    assert(inbox_insert(db, sid, "telegram", NULL, "plain message") > 0);

    /* peek exposes the column so callers can route on it */
    int count = 0;
    InboxItem *items = inbox_peek(db, sid, 10, &count);
    assert(count == 3);
    assert(items[0].source_ref && strcmp(items[0].source_ref, "jobname") == 0);
    assert(items[1].source_ref && strcmp(items[1].source_ref, "77") == 0);
    assert(items[2].source_ref == NULL);
    inbox_items_free(items, count);

    assert(inbox_consume_into_entries(db, sid, 10) == 3);

    int ecount = 0;
    Entry *entries = session_get_branch(db, sid, &ecount);
    assert(entries != NULL && ecount == 3);
    assert(strcmp(entries[0].message.content, "[cron: jobname] sweep the logs") == 0);
    assert(strcmp(entries[1].message.content,
                  "[sub-agent session 77] Sub-agent completed: ok") == 0);
    assert(strcmp(entries[2].message.content, "plain message") == 0);
    entry_branch_free(entries, ecount);

    /* The same provenance lands structurally in entries.data, so a fire's
     * outcome stays queryable after the prose is compacted away. A NULL
     * source_ref leaves the key out entirely rather than storing JSON null. */
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "SELECT json_extract(data,'$.source'), json_extract(data,'$.source_ref'),"
        "       json_type(data,'$.source_ref')"
        " FROM entries WHERE session_id=? AND role=1 ORDER BY id", -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "cron") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "jobname") == 0);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "agent_result") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "77") == 0);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "telegram") == 0);
    assert(sqlite3_column_type(s, 2) == SQLITE_NULL);   /* key omitted, not null */
    sqlite3_finalize(s);

    db_close(db);
    printf("  PASS test_inbox_consume_tags_source_ref\n");
}

static void test_inbox_consume_empty(void) {
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "inbox_consume_empty", NULL, -1, 0);
    assert(sid > 0);

    int consumed = inbox_consume_into_entries(db, sid, 10);
    assert(consumed == 0);

    db_close(db);
    printf("  PASS test_inbox_consume_empty\n");
}

static void test_inbox_consume_respects_limit(void) {
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "inbox_consume_limit", NULL, -1, 0);
    assert(sid > 0);

    inbox_insert(db, sid, "src", NULL, "a");
    inbox_insert(db, sid, "src", NULL, "b");
    inbox_insert(db, sid, "src", NULL, "c");

    /* Consume only 2 */
    int consumed = inbox_consume_into_entries(db, sid, 2);
    assert(consumed == 2);

    /* 1 still pending */
    int count = 0;
    InboxItem *items = inbox_peek(db, sid, 10, &count);
    assert(count == 1);
    assert(strcmp(items[0].payload, "c") == 0);
    inbox_items_free(items, count);

    db_close(db);
    printf("  PASS test_inbox_consume_respects_limit\n");
}

static void test_inbox_count(void) {
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "inbox_count", NULL, -1, 0);
    assert(sid > 0);

    /* Empty session → 0 */
    assert(inbox_count(db, sid) == 0);

    inbox_insert(db, sid, "telegram", NULL, "a");
    inbox_insert(db, sid, "cron", NULL, "b");
    assert(inbox_count(db, sid) == 2);

    /* Consume one, count drops */
    inbox_consume_into_entries(db, sid, 1);
    assert(inbox_count(db, sid) == 1);

    db_close(db);
    printf("  PASS test_inbox_count\n");
}

int main(void) {
    TEST_INIT();
    test_db_clean(DB_PATH);
    printf("test_inbox:\n");
    test_inbox_insert();
    test_inbox_peek_empty();
    test_inbox_peek_returns_unconsumed();
    test_inbox_peek_respects_limit();
    test_inbox_peek_session_isolation();
    test_inbox_consume_into_entries();
    test_inbox_consume_tags_source_ref();
    test_inbox_consume_empty();
    test_inbox_consume_respects_limit();
    test_inbox_count();
    test_db_clean(DB_PATH);
    printf("All inbox tests passed.\n");
    return 0;
}
