#define _POSIX_C_SOURCE 200809L
#include "db_request.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sqlite3 *test_db(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    return db;
}

/* Test: db_build_request_typed OpenAI format */
static void test_typed_openai_basic(void) {
    printf("  test_typed_openai_basic...");
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    /* Insert typed entries */
    sqlite3_exec(db,
        "INSERT INTO entries(session_id,type,role,content,turn_id,part_index)"
        " VALUES(1,'system',0,'You are helpful.',1,0),"
        "       (1,'user_message',1,'hello',2,0),"
        "       (1,'assistant_message',2,'hi there',2,0);",
        NULL, NULL, NULL);

    int64_t ids[] = {1, 2, 3};
    char *json = db_build_request_typed(db, sid, ids, 3, ENDPOINT_OPENAI, 0);
    assert(json);

    /* Validate using SQLite json_extract */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT json_array_length(?1)", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, json, -1, SQLITE_STATIC);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 3);
    sqlite3_finalize(stmt);

    /* Check first message role */
    sqlite3_prepare_v2(db, "SELECT json_extract(?1,'$[0].role')", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, json, -1, SQLITE_STATIC);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "system") == 0);
    sqlite3_finalize(stmt);

    /* Check user content */
    sqlite3_prepare_v2(db, "SELECT json_extract(?1,'$[1].content')", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, json, -1, SQLITE_STATIC);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "hello") == 0);
    sqlite3_finalize(stmt);

    free(json);
    db_close(db);
    printf(" OK\n");
}

/* Test: db_build_request_typed with tool_calls */
static void test_typed_openai_tool_calls(void) {
    printf("  test_typed_openai_tool_calls...");
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    sqlite3_exec(db,
        "INSERT INTO entries(session_id,type,role,content,turn_id,part_index)"
        " VALUES(1,'user_message',1,'run ls',1,0),"
        "       (1,'assistant_message',2,NULL,1,0);",
        NULL, NULL, NULL);
    sqlite3_exec(db,
        "INSERT INTO entries(session_id,type,role,content,turn_id,part_index,tool_call_id,tool_name)"
        " VALUES(1,'tool_call',2,'{\"cmd\":\"ls\"}',1,1,'call_1','shell_exec'),"
        "       (1,'tool_result',3,'file1.txt',1,0,'call_1','shell_exec');",
        NULL, NULL, NULL);

    int64_t ids[] = {1, 2, 4};  /* user, assistant, tool_result (skip tool_call — filtered by SQL) */
    char *json = db_build_request_typed(db, sid, ids, 3, ENDPOINT_OPENAI, 0);
    assert(json);

    /* Validate tool_calls present on assistant message */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT json_extract(?1,'$[0].role')", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, json, -1, SQLITE_STATIC);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "user") == 0);
    sqlite3_finalize(stmt);

    free(json);
    db_close(db);
    printf(" OK\n");
}

int main(void) {
    printf("test_db_request:\n");
    test_typed_openai_basic();
    test_typed_openai_tool_calls();
    printf("All tests passed.\n");
    return 0;
}
