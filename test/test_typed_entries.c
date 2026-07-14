#include "db.h"
#include "test_util.h"
#include "db_response.h"
#include "types.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sqlite3 *setup(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    session_create(db, "test", NULL, -1, 0);
    return db;
}

static void test_insert_typed_entries(void) {
    sqlite3 *db = setup();
    int64_t sid = 1;
    int64_t turn = 1;

    /* Insert reasoning entry */
    int64_t id1 = entry_append_typed(db, sid, turn, "reasoning", 0,
        "Let me think about this...", NULL, NULL, 0, STOP_REASON_NONE, NULL, 0, 0, 0);
    assert(id1 > 0);

    /* Insert assistant_message entry */
    int64_t id2 = entry_append_typed(db, sid, turn, "assistant_message", 1,
        "Here is my answer.", NULL, NULL, 0, STOP_REASON_STOP, "gpt-4o", 100, 50, 0);
    assert(id2 > 0);

    /* Insert tool_call entries */
    int64_t id3 = entry_append_typed(db, sid, turn, "tool_call", 2,
        "{\"path\":\"/tmp\"}", "call_abc", "file_read", 0, STOP_REASON_NONE, NULL, 0, 0, 0);
    assert(id3 > 0);

    int64_t id4 = entry_append_typed(db, sid, turn, "tool_call", 3,
        "{\"cmd\":\"ls\"}", "call_def", "shell_exec", 0, STOP_REASON_NONE, NULL, 0, 0, 0);
    assert(id4 > 0);

    /* Verify entries in DB */
    sqlite3_stmt *stmt;
    const char *sql = "SELECT type, part_index, content, tool_call_id, tool_name, role"
                      " FROM entries WHERE session_id=? AND turn_id=? ORDER BY part_index;";
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, sid);
    sqlite3_bind_int64(stmt, 2, turn);

    /* Row 0: reasoning */
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "reasoning") == 0);
    assert(sqlite3_column_int(stmt, 1) == 0);
    assert(strstr((const char *)sqlite3_column_text(stmt, 2), "think") != NULL);
    assert(sqlite3_column_int(stmt, 5) == 2); /* ROLE_ASSISTANT */

    /* Row 1: assistant_message */
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "assistant_message") == 0);
    assert(sqlite3_column_int(stmt, 1) == 1);
    assert(sqlite3_column_int(stmt, 5) == 2);

    /* Row 2: tool_call */
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "tool_call") == 0);
    assert(sqlite3_column_int(stmt, 1) == 2);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 3), "call_abc") == 0);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 4), "file_read") == 0);

    /* Row 3: tool_call */
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "tool_call") == 0);
    assert(sqlite3_column_int(stmt, 1) == 3);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 3), "call_def") == 0);

    assert(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    db_close(db);
    printf("  insert_typed_entries... PASS\n");
}

static void test_tool_result_typed(void) {
    sqlite3 *db = setup();
    int64_t sid = 1;
    int64_t turn = 1;

    /* Insert tool_result */
    int64_t id = entry_append_typed(db, sid, turn, "tool_result", 0,
        "file contents here", "call_abc", "file_read", 0, STOP_REASON_NONE, NULL, 0, 0, 0);
    assert(id > 0);

    /* Verify role derived correctly */
    sqlite3_stmt *stmt;
    assert(sqlite3_prepare_v2(db, "SELECT type, role, tool_call_id FROM entries WHERE id=?;",
           -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, id);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "tool_result") == 0);
    assert(sqlite3_column_int(stmt, 1) == 3); /* ROLE_TOOL */
    assert(strcmp((const char *)sqlite3_column_text(stmt, 2), "call_abc") == 0);
    sqlite3_finalize(stmt);
    db_close(db);
    printf("  tool_result_typed... PASS\n");
}

static void test_legacy_entry_gets_type(void) {
    sqlite3 *db = setup();
    int64_t sid = 1;

    /* Use legacy entry_append — should get type derived from role */
    Message msg = {.role = ROLE_USER, .content = "hello"};
    int64_t id = entry_append_with_turn(db, sid, &msg, 1);
    assert(id > 0);

    sqlite3_stmt *stmt;
    assert(sqlite3_prepare_v2(db, "SELECT type, part_index FROM entries WHERE id=?;",
           -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, id);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "user_message") == 0);
    assert(sqlite3_column_int(stmt, 1) == 0);
    sqlite3_finalize(stmt);

    /* Assistant message */
    Message amsg = {.role = ROLE_ASSISTANT, .content = "hi"};
    int64_t id2 = entry_append_with_turn(db, sid, &amsg, 1);
    assert(id2 > 0);
    assert(sqlite3_prepare_v2(db, "SELECT type FROM entries WHERE id=?;",
           -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, id2);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "assistant_message") == 0);
    sqlite3_finalize(stmt);

    db_close(db);
    printf("  legacy_entry_gets_type... PASS\n");
}

static void test_tool_calls_result_entry_id(void) {
    sqlite3 *db = setup();
    int64_t sid = 1;

    /* Insert a tool_call entry and corresponding tool_calls row */
    int64_t tc_entry = entry_append_typed(db, sid, 1, "tool_call", 0,
        "{\"x\":1}", "call_1", "test_tool", 0, STOP_REASON_NONE, NULL, 0, 0, 0);
    assert(tc_entry > 0);

    /* Insert into tool_calls table (workflow state only — args in entries.content) */
    sqlite3_stmt *stmt;
    const char *ins = "INSERT INTO tool_calls(session_id, entry_id, call_id, name)"
                      " VALUES(?,?,?,?);";
    assert(sqlite3_prepare_v2(db, ins, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, sid);
    sqlite3_bind_int64(stmt, 2, tc_entry);
    sqlite3_bind_text(stmt, 3, "call_1", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, "test_tool", -1, SQLITE_STATIC);
    assert(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);

    /* Insert tool_result entry */
    int64_t result_entry = entry_append_typed(db, sid, 1, "tool_result", 1,
        "result!", "call_1", "test_tool", 0, STOP_REASON_NONE, NULL, 0, 0, 0);
    assert(result_entry > 0);

    /* Update tool_calls with result_entry_id */
    const char *upd = "UPDATE tool_calls SET status='done', result_entry_id=? WHERE call_id=?;";
    assert(sqlite3_prepare_v2(db, upd, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, result_entry);
    sqlite3_bind_text(stmt, 2, "call_1", -1, SQLITE_STATIC);
    assert(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);

    /* Verify */
    const char *sel = "SELECT status, result_entry_id FROM tool_calls WHERE call_id='call_1';";
    assert(sqlite3_prepare_v2(db, sel, -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "done") == 0);
    assert(sqlite3_column_int64(stmt, 1) == result_entry);
    sqlite3_finalize(stmt);

    db_close(db);
    printf("  tool_calls_result_entry_id... PASS\n");
}

static void test_ingest_typed(void) {
    sqlite3 *db = setup();
    int64_t sid = 1;
    int64_t turn = db_next_turn_id(db, sid);

    const char *body =
        "{\"choices\":[{\"message\":{\"content\":\"Here are the results.\","
        "\"reasoning\":\"I need to think...\","
        "\"tool_calls\":[{\"id\":\"call_1\",\"function\":{\"name\":\"file_read\","
        "\"arguments\":\"{\\\"path\\\":\\\"/tmp\\\"}\"}},"
        "{\"id\":\"call_2\",\"function\":{\"name\":\"shell_exec\","
        "\"arguments\":\"{\\\"cmd\\\":\\\"ls\\\"}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}],"
        "\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":50,\"cost\":0.0000015}}";

    TypedIngestResult res;
    LlmRespStatus st = db_ingest_response(db, sid, turn, "gpt-4o", ENDPOINT_OPENAI,
                                          body, NULL, &res);
    assert(st == LLM_RESP_OK);
    assert(res.assistant_entry_id > 0);
    assert(res.prompt_tokens == 100);
    assert(res.completion_tokens == 50);
    assert(res.cost_nano == 1500);

    /* Verify entries: should be reasoning(0) + assistant(1) + 2 tool_calls(2,3) */
    sqlite3_stmt *stmt;
    const char *sql = "SELECT type, part_index, content FROM entries"
                      " WHERE session_id=? AND turn_id=? ORDER BY part_index;";
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, sid);
    sqlite3_bind_int64(stmt, 2, turn);

    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "reasoning") == 0);
    assert(sqlite3_column_int(stmt, 1) == 0);

    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "assistant_message") == 0);
    assert(sqlite3_column_int(stmt, 1) == 1);

    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "tool_call") == 0);
    assert(sqlite3_column_int(stmt, 1) == 2);

    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "tool_call") == 0);
    assert(sqlite3_column_int(stmt, 1) == 3);

    assert(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);

    /* Verify tool_calls table */
    const char *tc_sql = "SELECT call_id, name, status FROM tool_calls"
                         " WHERE session_id=? ORDER BY rowid;";
    assert(sqlite3_prepare_v2(db, tc_sql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, sid);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "call_1") == 0);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 1), "file_read") == 0);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 2), "pending") == 0);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "call_2") == 0);
    sqlite3_finalize(stmt);

    db_close(db);
    printf("  ingest_typed... PASS\n");
}

int main(void) {
    printf("test_typed_entries:\n");
    test_insert_typed_entries();
    test_tool_result_typed();
    test_legacy_entry_gets_type();
    test_tool_calls_result_entry_id();
    test_ingest_typed();
    printf("\n5/5 tests passed.\n");
    return 0;
}
