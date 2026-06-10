/* Test: tool_calls status helpers (db_tool_call_set_status, db_tool_call_get_pending) */
#include "db.h"
#include "db_response.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static sqlite3 *setup(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    return db;
}

static void test_get_pending_empty(void) {
    sqlite3 *db = setup();
    int count = 0;
    PendingToolCall *list = db_tool_call_get_pending(db, 1, &count);
    assert(count == 0);
    assert(list == NULL);
    db_close(db);
    printf("  PASS test_get_pending_empty\n");
}

static void test_insert_and_get_pending(void) {
    sqlite3 *db = setup();
    /* Create session + entry for tool_calls to reference */
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    assert(sid > 0);
    Message msg = {.role = ROLE_ASSISTANT, .content = "hello"};
    int64_t eid = entry_append_with_turn(db, sid, &msg, 1);
    assert(eid > 0);

    /* Insert tool_call rows */
    const char *sql = "INSERT INTO tool_calls(session_id, entry_id, call_id, name, arguments)"
                      " VALUES(?, ?, 'call_1', 'file_read', '{\"path\":\"x.txt\"}');";
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, sid);
    sqlite3_bind_int64(stmt, 2, eid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* Should appear as pending */
    int count = 0;
    PendingToolCall *list = db_tool_call_get_pending(db, sid, &count);
    assert(count == 1);
    assert(strcmp(list[0].call_id, "call_1") == 0);
    assert(strcmp(list[0].name, "file_read") == 0);
    assert(strstr(list[0].arguments, "x.txt") != NULL);
    db_tool_call_free_pending(list, count);

    db_close(db);
    printf("  PASS test_insert_and_get_pending\n");
}

static void test_set_status(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    Message msg = {.role = ROLE_ASSISTANT, .content = "hi"};
    int64_t eid = entry_append_with_turn(db, sid, &msg, 1);

    const char *sql = "INSERT INTO tool_calls(session_id, entry_id, call_id, name, arguments)"
                      " VALUES(?, ?, 'call_2', 'web_fetch', '{}');";
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, sid);
    sqlite3_bind_int64(stmt, 2, eid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* Set to awaiting_approval */
    int rc = db_tool_call_set_status(db, sid, "call_2", "awaiting_approval", NULL);
    assert(rc == 0);

    /* Should no longer be pending */
    int count = 0;
    PendingToolCall *list = db_tool_call_get_pending(db, sid, &count);
    assert(count == 0);
    assert(list == NULL);

    /* Resolve it */
    rc = db_tool_call_set_status(db, sid, "call_2", "done", "admin");
    assert(rc == 0);

    /* Verify resolved_by was set */
    sqlite3_prepare_v2(db, "SELECT resolved_by FROM tool_calls WHERE call_id='call_2'",
                       -1, &stmt, NULL);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    const char *by = (const char *)sqlite3_column_text(stmt, 0);
    assert(by && strcmp(by, "admin") == 0);
    sqlite3_finalize(stmt);

    db_close(db);
    printf("  PASS test_set_status\n");
}

int main(void) {
    printf("test_tool_calls_status:\n");
    test_get_pending_empty();
    test_insert_and_get_pending();
    test_set_status();
    printf("ALL PASSED\n");
    return 0;
}
