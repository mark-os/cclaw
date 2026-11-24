#include "db.h"
#include "test_util.h"
#include <assert.h>

/* Delivery through the inbox: items drain oldest first into the session's
 * branch as a chained run of user entries, exactly once, per session, and a
 * partial drain leaves the rest queued. */
static void test_inbox_drain(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "inbox_a", NULL, -1, 0);
    int64_t other = session_create(db, "inbox_b", NULL, -1, 0);
    assert(sid > 0 && other > 0);
    assert(inbox_consume_into_entries(db, sid, 10) == 0);

    inbox_insert(db, sid, "telegram", NULL, "hello");
    inbox_insert(db, sid, "telegram", NULL, "world");
    inbox_insert(db, sid, "cron", NULL, "tick");
    inbox_insert(db, other, "src", NULL, "for_other");
    assert(inbox_count(db, sid) == 3);

    int count = 0;
    InboxItem *items = inbox_peek(db, sid, 2, &count);
    assert(count == 2);
    assert(strcmp(items[0].payload, "hello") == 0);
    inbox_items_free(items, count);

    assert(inbox_consume_into_entries(db, sid, 2) == 2);
    items = inbox_peek(db, sid, 10, &count);
    assert(count == 1 && strcmp(items[0].payload, "tick") == 0);
    inbox_items_free(items, count);
    assert(inbox_consume_into_entries(db, sid, 10) == 1);
    assert(inbox_count(db, sid) == 0);
    assert(inbox_count(db, other) == 1);

    int ecount = 0;
    Entry *entries = session_get_branch(db, sid, &ecount);
    assert(entries && ecount == 3);
    assert(entries[0].message.role == ROLE_USER);
    assert(strcmp(entries[0].message.content, "hello") == 0);
    assert(strcmp(entries[1].message.content, "world") == 0);
    assert(strcmp(entries[2].message.content, "tick") == 0);
    assert(entries[0].parent_id == -1);
    assert(entries[1].parent_id == entries[0].id);
    assert(entries[2].parent_id == entries[1].id);
    entry_branch_free(entries, ecount);
    db_close(db);
}

/* source_ref becomes a provenance tag on the entry at drain time — the model
 * must see which job/child spoke without a join the referent may not survive.
 * A NULL source_ref (every writer but agent_result today) drains verbatim. */
static void test_inbox_consume_tags_source_ref(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "inbox_source_ref", NULL, -1, 0);
    assert(sid > 0);

    assert(inbox_insert(db, sid, "cron", "jobname", "sweep the logs") > 0);
    assert(inbox_insert(db, sid, "agent_result", "77", "Sub-agent completed: ok") > 0);
    assert(inbox_insert(db, sid, "telegram", NULL, "plain message") > 0);

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
}

int main(void) {
    TEST_INIT();
    RUN(test_inbox_drain);
    RUN(test_inbox_consume_tags_source_ref);
    return 0;
}
