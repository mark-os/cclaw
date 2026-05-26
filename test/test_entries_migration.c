#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_DB "/tmp/test_entries_migration.sqlite"

/* T164: verify split-column migration from old JSON data schema */
static void test_entries_split_column_migration(void) {
    unlink(TEST_DB);

    sqlite3 *db = NULL;
    assert(sqlite3_open(TEST_DB, &db) == SQLITE_OK);
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA busy_timeout=5000;", NULL, NULL, NULL);

    /* Create old-style entries table with data TEXT NOT NULL + GENERATED columns */
    const char *old_schema =
        "CREATE TABLE entries ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  session_id INTEGER NOT NULL,"
        "  parent_id INTEGER NOT NULL DEFAULT -1,"
        "  turn_id INTEGER,"
        "  data TEXT NOT NULL,"
        "  token_estimate INTEGER,"
        "  content_bytes INTEGER,"
        "  role TEXT GENERATED ALWAYS AS (json_extract(data, '$.role')) STORED,"
        "  created_at INTEGER NOT NULL DEFAULT (unixepoch())"
        ");"
        "CREATE TABLE sessions ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT,"
        "  leaf_id INTEGER DEFAULT -1,"
        "  agent_name TEXT,"
        "  parent_session_id INTEGER DEFAULT -1,"
        "  depth INTEGER NOT NULL DEFAULT 0,"
        "  state TEXT NOT NULL DEFAULT 'idle',"
        "  last_route TEXT,"
        "  created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
        "  updated_at INTEGER NOT NULL DEFAULT (unixepoch())"
        ");";
    char *err = NULL;
    assert(sqlite3_exec(db, old_schema, NULL, NULL, &err) == SQLITE_OK);

    /* Insert a session */
    sqlite3_exec(db, "INSERT INTO sessions (name) VALUES ('test');", NULL, NULL, NULL);

    /* Insert old-format entries: user, assistant w/ tool_calls, tool_result */
    const char *user_data = "{\"role\":\"user\",\"content\":\"hello world\"}";
    const char *asst_data = "{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"I will help\"},{\"type\":\"tool_call\",\"id\":\"tc_1\",\"name\":\"shell_exec\",\"arguments\":{\"cmd\":\"ls\"}}]}";
    const char *tool_data = "{\"role\":\"tool_result\",\"content\":\"file1.c\\nfile2.c\",\"tool_call_id\":\"tc_1\"}";
    const char *asst2_data = "{\"role\":\"assistant\",\"content\":\"Done!\",\"stop_reason\":\"stop\"}";

    sqlite3_stmt *ins;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO entries (session_id, parent_id, turn_id, data, token_estimate, content_bytes) VALUES (1,?,?,?,?,?)",
        -1, &ins, NULL) == SQLITE_OK);

    /* user entry */
    sqlite3_bind_int64(ins, 1, -1);
    sqlite3_bind_int64(ins, 2, 1);
    sqlite3_bind_text(ins, 3, user_data, -1, SQLITE_STATIC);
    sqlite3_bind_int(ins, 4, 3);
    sqlite3_bind_int(ins, 5, 11);
    assert(sqlite3_step(ins) == SQLITE_DONE);
    sqlite3_reset(ins);

    /* assistant w/ tool_call */
    sqlite3_bind_int64(ins, 1, 1);
    sqlite3_bind_int64(ins, 2, 1);
    sqlite3_bind_text(ins, 3, asst_data, -1, SQLITE_STATIC);
    sqlite3_bind_int(ins, 4, 10);
    sqlite3_bind_int(ins, 5, 40);
    assert(sqlite3_step(ins) == SQLITE_DONE);
    sqlite3_reset(ins);

    /* tool_result */
    sqlite3_bind_int64(ins, 1, 2);
    sqlite3_bind_int64(ins, 2, 1);
    sqlite3_bind_text(ins, 3, tool_data, -1, SQLITE_STATIC);
    sqlite3_bind_int(ins, 4, 4);
    sqlite3_bind_int(ins, 5, 15);
    assert(sqlite3_step(ins) == SQLITE_DONE);
    sqlite3_reset(ins);

    /* final assistant */
    sqlite3_bind_int64(ins, 1, 3);
    sqlite3_bind_int64(ins, 2, 1);
    sqlite3_bind_text(ins, 3, asst2_data, -1, SQLITE_STATIC);
    sqlite3_bind_int(ins, 4, 2);
    sqlite3_bind_int(ins, 5, 5);
    assert(sqlite3_step(ins) == SQLITE_DONE);
    sqlite3_finalize(ins);

    sqlite3_close(db);

    /* Re-open with db_open — triggers migration */
    db = db_open(TEST_DB);
    assert(db != NULL);

    /* Verify user entry: role=1, content="hello world" */
    sqlite3_stmt *sel;
    assert(sqlite3_prepare_v2(db,
        "SELECT role, content, tool_calls, tool_call_id, stop_reason, tool_call_count FROM entries WHERE id=1",
        -1, &sel, NULL) == SQLITE_OK);
    assert(sqlite3_step(sel) == SQLITE_ROW);
    assert(sqlite3_column_int(sel, 0) == 1); /* ROLE_USER */
    assert(strcmp((const char *)sqlite3_column_text(sel, 1), "hello world") == 0);
    assert(sqlite3_column_type(sel, 2) == SQLITE_NULL); /* no tool_calls */
    assert(sqlite3_column_type(sel, 3) == SQLITE_NULL); /* no tool_call_id */
    assert(sqlite3_column_int(sel, 4) == 0); /* no stop_reason */
    assert(sqlite3_column_int(sel, 5) == 0); /* tool_call_count=0 */
    sqlite3_finalize(sel);

    /* Verify assistant w/ tool_call: role=2, content="I will help", tool_calls has 1 item */
    assert(sqlite3_prepare_v2(db,
        "SELECT role, content, tool_calls, stop_reason, tool_call_count FROM entries WHERE id=2",
        -1, &sel, NULL) == SQLITE_OK);
    assert(sqlite3_step(sel) == SQLITE_ROW);
    assert(sqlite3_column_int(sel, 0) == 2); /* ROLE_ASSISTANT */
    assert(strcmp((const char *)sqlite3_column_text(sel, 1), "I will help") == 0);
    const char *tc = (const char *)sqlite3_column_text(sel, 2);
    assert(tc != NULL);
    assert(strstr(tc, "\"id\":\"tc_1\"") != NULL);
    assert(strstr(tc, "\"name\":\"shell_exec\"") != NULL);
    assert(strstr(tc, "\"args\"") != NULL);
    assert(sqlite3_column_int(sel, 3) == 0); /* no stop_reason */
    assert(sqlite3_column_int(sel, 4) == 1); /* tool_call_count=1 */
    sqlite3_finalize(sel);

    /* Verify tool_result: role=3, content="file1.c\nfile2.c", tool_call_id="tc_1" */
    assert(sqlite3_prepare_v2(db,
        "SELECT role, content, tool_call_id FROM entries WHERE id=3",
        -1, &sel, NULL) == SQLITE_OK);
    assert(sqlite3_step(sel) == SQLITE_ROW);
    assert(sqlite3_column_int(sel, 0) == 3); /* ROLE_TOOL */
    assert(strstr((const char *)sqlite3_column_text(sel, 1), "file1.c") != NULL);
    assert(strcmp((const char *)sqlite3_column_text(sel, 2), "tc_1") == 0);
    sqlite3_finalize(sel);

    /* Verify final assistant: role=2, stop_reason=1 (stop) */
    assert(sqlite3_prepare_v2(db,
        "SELECT role, content, stop_reason FROM entries WHERE id=4",
        -1, &sel, NULL) == SQLITE_OK);
    assert(sqlite3_step(sel) == SQLITE_ROW);
    assert(sqlite3_column_int(sel, 0) == 2);
    assert(strcmp((const char *)sqlite3_column_text(sel, 1), "Done!") == 0);
    assert(sqlite3_column_int(sel, 2) == 1); /* STOP_REASON_STOP */
    sqlite3_finalize(sel);

    /* Verify data column retained (nullable, for rollback) */
    assert(sqlite3_prepare_v2(db,
        "SELECT data FROM entries WHERE id=1",
        -1, &sel, NULL) == SQLITE_OK);
    assert(sqlite3_step(sel) == SQLITE_ROW);
    /* Old data preserved */
    assert(sqlite3_column_type(sel, 0) == SQLITE_TEXT);
    sqlite3_finalize(sel);

    /* Verify FTS5 works on migrated content */
    assert(sqlite3_prepare_v2(db,
        "SELECT rowid FROM entries_fts WHERE entries_fts MATCH 'hello'",
        -1, &sel, NULL) == SQLITE_OK);
    assert(sqlite3_step(sel) == SQLITE_ROW);
    assert(sqlite3_column_int64(sel, 0) == 1);
    sqlite3_finalize(sel);

    /* Verify indexes exist */
    assert(sqlite3_prepare_v2(db,
        "SELECT 1 FROM sqlite_master WHERE type='index' AND name='idx_entries_session'",
        -1, &sel, NULL) == SQLITE_OK);
    assert(sqlite3_step(sel) == SQLITE_ROW);
    sqlite3_finalize(sel);

    db_close(db);
    unlink(TEST_DB);
    printf("  PASS test_entries_split_column_migration\n");
}

/* T164: fresh DB creates entries with split columns directly (no data JSON) */
static void test_fresh_db_split_columns(void) {
    unlink(TEST_DB);
    sqlite3 *db = db_open(TEST_DB);
    assert(db != NULL);

    /* Verify entries table has split columns, no GENERATED */
    sqlite3_stmt *stmt;
    assert(sqlite3_prepare_v2(db,
        "SELECT sql FROM sqlite_master WHERE type='table' AND name='entries'",
        -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    const char *sql = (const char *)sqlite3_column_text(stmt, 0);
    assert(strstr(sql, "role INTEGER") != NULL);
    assert(strstr(sql, "content TEXT") != NULL);
    assert(strstr(sql, "tool_calls TEXT") != NULL);
    assert(strstr(sql, "GENERATED ALWAYS") == NULL);
    sqlite3_finalize(stmt);

    db_close(db);
    unlink(TEST_DB);
    printf("  PASS test_fresh_db_split_columns\n");
}

int main(void) {
    printf("test_entries_migration:\n");
    test_entries_split_column_migration();
    test_fresh_db_split_columns();
    printf("All entries migration tests passed.\n");
    return 0;
}
