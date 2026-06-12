#define _POSIX_C_SOURCE 200809L
#include "db_response.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sqlite3 *test_db(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    /* Create tool_calls table (normally done by db_open_agent) */
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS tool_calls ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  session_id INTEGER NOT NULL,"
        "  entry_id INTEGER NOT NULL,"
        "  call_id TEXT NOT NULL,"
        "  name TEXT NOT NULL,"
        "  arguments TEXT,"
        "  status TEXT NOT NULL DEFAULT 'pending',"
        "  result_entry_id INTEGER"
        ");", NULL, NULL, NULL);
    return db;
}

/* Test: db_ingest_typed creates entries correctly */
static void test_ingest_typed(void) {
    printf("  test_ingest_typed...");
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message msg = {.role = ROLE_USER, .content = "do it"};
    entry_append_with_turn(db, sid, &msg, 1);

    const char *tc_ids[] = {"call_typed"};
    const char *tc_names[] = {"shell_exec"};
    const char *tc_args[] = {"{\"cmd\":\"ls\"}"};

    TypedIngestResult result;
    int rc = db_ingest_typed(db, sid, 1, "deepseek-v4",
                             "Let me list files.", NULL, "tool_calls",
                             100, 50, 1500,
                             tc_ids, tc_names, tc_args, 1, &result);
    assert(rc == 0);
    assert(result.assistant_entry_id > 0);
    assert(result.tc_count == 1);
    assert(result.tc_entry_ids != NULL);
    assert(result.tc_entry_ids[0] > 0);

    /* Verify tool_calls table */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db,
        "SELECT call_id, name, arguments FROM tool_calls WHERE call_id='call_typed';",
        -1, &stmt, NULL);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "call_typed") == 0);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 1), "shell_exec") == 0);
    sqlite3_finalize(stmt);

    free(result.tc_entry_ids);
    db_close(db);
    printf(" OK\n");
}

/* Test: tool_call_complete marks status */
static void test_tool_call_complete(void) {
    printf("  test_tool_call_complete...");
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message msg = {.role = ROLE_USER, .content = "do it"};
    entry_append_with_turn(db, sid, &msg, 1);

    const char *tc_ids[] = {"call_x"};
    const char *tc_names[] = {"shell_exec"};
    const char *tc_args[] = {"{\"cmd\":\"echo hi\"}"};

    TypedIngestResult result;
    db_ingest_typed(db, sid, 1, "m", NULL, NULL, "tool_calls",
                    0, 0, 0, tc_ids, tc_names, tc_args, 1, &result);

    /* Mark complete */
    int rc = db_tool_call_complete(db, result.tc_entry_ids[0], "call_x");
    assert(rc == 0);

    /* Verify status changed */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT status FROM tool_calls WHERE call_id='call_x';",
                       -1, &stmt, NULL);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "done") == 0);
    sqlite3_finalize(stmt);

    /* Non-existent call_id returns -1 */
    rc = db_tool_call_complete(db, result.tc_entry_ids[0], "no_such");
    assert(rc == -1);

    free(result.tc_entry_ids);
    db_close(db);
    printf(" OK\n");
}

int main(void) {
    printf("test_db_response:\n");
    test_ingest_typed();
    test_tool_call_complete();
    printf("  ALL PASSED\n");
    return 0;
}
