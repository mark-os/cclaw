/* T201: PENDING entry dispatch — daemon finds PENDING tool_result,
 * reads matching tool_call args from assistant entry, updates on completion.
 * V77, V78, V79, V87 */
#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sqlite3 *setup(void) {
    sqlite3 *db = db_open(":memory:");
    assert(db);
    return db;
}

/* Helper: insert assistant entry with tool_calls JSON */
static int64_t insert_assistant_with_tool_calls(sqlite3 *db, int64_t sid,
                                                 int64_t turn_id, const char *tc_json) {
    const char *sql =
        "INSERT INTO entries (session_id, parent_id, turn_id, role, tool_calls, tool_call_count)"
        " VALUES (?, (SELECT leaf_id FROM sessions WHERE id=?), ?, 2, ?, 1);";
    sqlite3_stmt *stmt;
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, sid);
    sqlite3_bind_int64(stmt, 2, sid);
    sqlite3_bind_int64(stmt, 3, turn_id);
    sqlite3_bind_text(stmt, 4, tc_json, -1, SQLITE_STATIC);
    assert(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    int64_t eid = sqlite3_last_insert_rowid(db);
    session_set_leaf(db, sid, eid);
    return eid;
}

/* Helper: insert PENDING tool_result entry */
static int64_t insert_pending_entry(sqlite3 *db, int64_t sid,
                                     int64_t turn_id, const char *tc_id) {
    const char *sql =
        "INSERT INTO entries (session_id, parent_id, turn_id, role, content, tool_call_id)"
        " VALUES (?, (SELECT leaf_id FROM sessions WHERE id=?), ?, 3, 'PENDING', ?);";
    sqlite3_stmt *stmt;
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, sid);
    sqlite3_bind_int64(stmt, 2, sid);
    sqlite3_bind_int64(stmt, 3, turn_id);
    sqlite3_bind_text(stmt, 4, tc_id, -1, SQLITE_STATIC);
    assert(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    int64_t eid = sqlite3_last_insert_rowid(db);
    session_set_leaf(db, sid, eid);
    return eid;
}

/* Test: find PENDING entry by session_id */
static void test_find_pending(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    /* Insert assistant with tool_call */
    const char *tc = "[{\"id\":\"call_abc\",\"name\":\"launch_agent\","
                     "\"args\":{\"task\":\"do stuff\",\"background\":false}}]";
    insert_assistant_with_tool_calls(db, sid, 1, tc);

    /* Insert PENDING tool_result */
    int64_t peid = insert_pending_entry(db, sid, 1, "call_abc");
    assert(peid > 0);

    /* Find it */
    const char *fsql =
        "SELECT id, tool_call_id FROM entries"
        " WHERE session_id=? AND role=3 AND content='PENDING'"
        " ORDER BY id DESC LIMIT 1;";
    sqlite3_stmt *stmt;
    assert(sqlite3_prepare_v2(db, fsql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, sid);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int64(stmt, 0) == peid);
    const char *tc_id = (const char *)sqlite3_column_text(stmt, 1);
    assert(strcmp(tc_id, "call_abc") == 0);
    sqlite3_finalize(stmt);

    db_close(db);
    printf("  PASS test_find_pending\n");
}

/* Test: read tool_call args from assistant entry by tool_call_id */
static void test_read_tool_call_args(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    const char *tc = "[{\"id\":\"call_xyz\",\"name\":\"approval_request\","
                     "\"args\":{\"type\":\"whitelist_host\",\"payload\":{\"host\":\"x.com\"}}}]";
    insert_assistant_with_tool_calls(db, sid, 1, tc);

    /* Read tool_calls from last assistant entry */
    const char *sql =
        "SELECT tool_calls FROM entries"
        " WHERE session_id=? AND role=2 AND tool_calls IS NOT NULL"
        " ORDER BY id DESC LIMIT 1;";
    sqlite3_stmt *stmt;
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, sid);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    const char *tc_json = (const char *)sqlite3_column_text(stmt, 0);
    assert(tc_json);
    assert(strstr(tc_json, "call_xyz") != NULL);
    assert(strstr(tc_json, "whitelist_host") != NULL);
    sqlite3_finalize(stmt);

    db_close(db);
    printf("  PASS test_read_tool_call_args\n");
}

/* Test: UPDATE PENDING entry with real result */
static void test_update_pending(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    const char *tc = "[{\"id\":\"call_upd\",\"name\":\"launch_agent\","
                     "\"args\":{\"task\":\"hello\"}}]";
    insert_assistant_with_tool_calls(db, sid, 1, tc);
    int64_t peid = insert_pending_entry(db, sid, 1, "call_upd");

    /* Update PENDING → real result */
    const char *usql = "UPDATE entries SET content=? WHERE id=? AND content='PENDING';";
    sqlite3_stmt *stmt;
    assert(sqlite3_prepare_v2(db, usql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, "sub-agent result here", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, peid);
    assert(sqlite3_step(stmt) == SQLITE_DONE);
    assert(sqlite3_changes(db) == 1);
    sqlite3_finalize(stmt);

    /* Verify content updated */
    const char *vsql = "SELECT content FROM entries WHERE id=?;";
    assert(sqlite3_prepare_v2(db, vsql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, peid);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    const char *content = (const char *)sqlite3_column_text(stmt, 0);
    assert(strcmp(content, "sub-agent result here") == 0);
    sqlite3_finalize(stmt);

    /* Verify no more PENDING entries */
    const char *csql = "SELECT COUNT(*) FROM entries WHERE session_id=? AND content='PENDING';";
    assert(sqlite3_prepare_v2(db, csql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, sid);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 0);
    sqlite3_finalize(stmt);

    db_close(db);
    printf("  PASS test_update_pending\n");
}

/* Test: PENDING entry not found when none exists */
static void test_no_pending(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    const char *sql =
        "SELECT id FROM entries WHERE session_id=? AND role=3 AND content='PENDING';";
    sqlite3_stmt *stmt;
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, sid);
    assert(sqlite3_step(stmt) != SQLITE_ROW);
    sqlite3_finalize(stmt);

    db_close(db);
    printf("  PASS test_no_pending\n");
}

int main(void) {
    printf("test_pending_dispatch (T201):\n");
    test_find_pending();
    test_read_tool_call_args();
    test_update_pending();
    test_no_pending();
    printf("All T201 pending dispatch tests passed.\n");
    return 0;
}
