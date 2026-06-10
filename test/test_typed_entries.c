#include "db.h"
#include "db_response.h"
#include "db_request.h"
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
    int64_t id = entry_append(db, sid, &msg);
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
    int64_t id2 = entry_append(db, sid, &amsg);
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

    /* Insert into tool_calls table */
    sqlite3_stmt *stmt;
    const char *ins = "INSERT INTO tool_calls(session_id, entry_id, call_id, name, arguments)"
                      " VALUES(?,?,?,?,?);";
    assert(sqlite3_prepare_v2(db, ins, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, sid);
    sqlite3_bind_int64(stmt, 2, tc_entry);
    sqlite3_bind_text(stmt, 3, "call_1", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, "test_tool", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, "{\"x\":1}", -1, SQLITE_STATIC);
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

    const char *tc_ids[] = {"call_1", "call_2"};
    const char *tc_names[] = {"file_read", "shell_exec"};
    const char *tc_args[] = {"{\"path\":\"/tmp\"}", "{\"cmd\":\"ls\"}"};

    TypedIngestResult res;
    int rc = db_ingest_typed(db, sid, turn, "gpt-4o",
        "Here are the results.", "I need to think...",
        "tool_calls", 100, 50, 1500,
        tc_ids, tc_names, tc_args, 2, &res);
    assert(rc == 0);
    assert(res.assistant_entry_id > 0);
    assert(res.tc_count == 2);
    assert(res.tc_entry_ids[0] > 0);
    assert(res.tc_entry_ids[1] > 0);

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

    free(res.tc_entry_ids);
    db_close(db);
    printf("  ingest_typed... PASS\n");
}

static void test_openai_egress(void) {
    sqlite3 *db = setup();
    int64_t sid = 1;

    /* Build a conversation: system + user + assistant(with tool calls) + tool_result */
    int64_t t1 = 1;
    int64_t id1 = entry_append_typed(db, sid, t1, "system", 0, "You are helpful.", NULL, NULL, 0, STOP_REASON_NONE, NULL, 0, 0, 0);
    int64_t t2 = 2;
    int64_t id2 = entry_append_typed(db, sid, t2, "user_message", 0, "List files", NULL, NULL, 0, STOP_REASON_NONE, NULL, 0, 0, 0);
    int64_t t3 = 3;
    int64_t id3 = entry_append_typed(db, sid, t3, "assistant_message", 1, "Let me check.", NULL, NULL, 0, STOP_REASON_TOOL_USE, "gpt-4o", 0, 0, 0);
    int64_t id4 = entry_append_typed(db, sid, t3, "tool_call", 2, "{\"cmd\":\"ls\"}", "call_1", "shell_exec", 0, STOP_REASON_NONE, NULL, 0, 0, 0);
    int64_t t4 = 4;
    int64_t id5 = entry_append_typed(db, sid, t4, "tool_result", 0, "file1.c\nfile2.c", "call_1", "shell_exec", 0, STOP_REASON_NONE, NULL, 0, 0, 0);

    int64_t ids[] = {id1, id2, id3, id4, id5};
    char *json = db_build_request_typed(db, sid, ids, 5, ENDPOINT_OPENAI, 0);
    assert(json != NULL);

    /* Verify structure */
    assert(strstr(json, "\"role\":\"system\"") != NULL);
    assert(strstr(json, "\"role\":\"user\"") != NULL);
    assert(strstr(json, "\"role\":\"assistant\"") != NULL);
    assert(strstr(json, "\"tool_calls\"") != NULL);
    assert(strstr(json, "\"id\":\"call_1\"") != NULL);
    assert(strstr(json, "\"name\":\"shell_exec\"") != NULL);
    assert(strstr(json, "\"role\":\"tool\"") != NULL);
    assert(strstr(json, "\"tool_call_id\":\"call_1\"") != NULL);
    /* Reasoning should NOT appear */
    assert(strstr(json, "reasoning") == NULL);

    free(json);
    db_close(db);
    printf("  openai_egress... PASS\n");
}

static void test_responses_egress(void) {
    sqlite3 *db = setup();
    int64_t sid = 1;

    int64_t id1 = entry_append_typed(db, sid, 1, "system", 0, "Be helpful.", NULL, NULL, 0, STOP_REASON_NONE, NULL, 0, 0, 0);
    int64_t id2 = entry_append_typed(db, sid, 2, "user_message", 0, "Hello", NULL, NULL, 0, STOP_REASON_NONE, NULL, 0, 0, 0);
    int64_t id3 = entry_append_typed(db, sid, 3, "assistant_message", 1, "Hi!", NULL, NULL, 0, STOP_REASON_STOP, NULL, 0, 0, 0);
    int64_t id4 = entry_append_typed(db, sid, 3, "tool_call", 2, "{\"q\":\"test\"}", "call_x", "search", 0, STOP_REASON_NONE, NULL, 0, 0, 0);
    int64_t id5 = entry_append_typed(db, sid, 4, "tool_result", 0, "found it", "call_x", "search", 0, STOP_REASON_NONE, NULL, 0, 0, 0);

    int64_t ids[] = {id1, id2, id3, id4, id5};
    char *json = db_build_request_typed(db, sid, ids, 5, ENDPOINT_RESPONSES, 0);
    assert(json != NULL);

    /* Responses API uses type-based items */
    assert(strstr(json, "\"type\":\"message\"") != NULL);
    assert(strstr(json, "\"role\":\"developer\"") != NULL); /* system → developer */
    assert(strstr(json, "\"role\":\"user\"") != NULL);
    assert(strstr(json, "\"type\":\"function_call\"") != NULL);
    assert(strstr(json, "\"call_id\":\"call_x\"") != NULL);
    assert(strstr(json, "\"name\":\"search\"") != NULL);
    assert(strstr(json, "\"type\":\"function_call_output\"") != NULL);
    assert(strstr(json, "\"output\":\"found it\"") != NULL);

    free(json);
    db_close(db);
    printf("  responses_egress... PASS\n");
}

static void test_gemini_egress(void) {
    sqlite3 *db = setup();
    int64_t sid = 1;

    int64_t id1 = entry_append_typed(db, sid, 1, "system", 0, "You help.", NULL, NULL, 0, STOP_REASON_NONE, NULL, 0, 0, 0);
    (void)id1; /* system entries handled separately by caller */
    int64_t id2 = entry_append_typed(db, sid, 2, "user_message", 0, "Read /tmp", NULL, NULL, 0, STOP_REASON_NONE, NULL, 0, 0, 0);
    int64_t id3 = entry_append_typed(db, sid, 3, "assistant_message", 1, "OK", NULL, NULL, 0, STOP_REASON_TOOL_USE, NULL, 0, 0, 0);
    int64_t id4 = entry_append_typed(db, sid, 3, "tool_call", 2, "{\"path\":\"/tmp\"}", "call_g1", "file_read", 0, STOP_REASON_NONE, NULL, 0, 0, 0);
    int64_t id5 = entry_append_typed(db, sid, 4, "tool_result", 0, "contents", "call_g1", "file_read", 0, STOP_REASON_NONE, NULL, 0, 0, 0);

    /* System is excluded from contents (caller handles systemInstruction) */
    int64_t ids[] = {id2, id3, id4, id5};
    char *json = db_build_request_typed(db, sid, ids, 4, ENDPOINT_GEMINI, 0);
    assert(json != NULL);

    /* Verify Gemini structure */
    assert(strstr(json, "\"role\":\"user\"") != NULL);
    assert(strstr(json, "\"role\":\"model\"") != NULL);
    assert(strstr(json, "\"functionCall\"") != NULL);
    assert(strstr(json, "\"name\":\"file_read\"") != NULL);
    assert(strstr(json, "\"functionResponse\"") != NULL);
    assert(strstr(json, "\"parts\"") != NULL);
    /* Args should be an object, not a string */
    assert(strstr(json, "\"args\":{\"path\":\"/tmp\"}") != NULL);

    free(json);
    db_close(db);
    printf("  gemini_egress... PASS\n");
}

static void test_gemini_interactions_egress(void) {
    sqlite3 *db = setup();
    int64_t sid = 1;

    /* First turn: user + assistant */
    int64_t id1 = entry_append_typed(db, sid, 1, "user_message", 0, "Hello", NULL, NULL, 0, STOP_REASON_NONE, NULL, 0, 0, 0);
    int64_t id2 = entry_append_typed(db, sid, 2, "assistant_message", 0, "Hi there!", NULL, NULL, 0, STOP_REASON_STOP, NULL, 0, 0, 0);

    /* Full history (no last_synced_entry_id set) */
    int64_t ids_all[] = {id1, id2};
    char *json = db_build_request_typed(db, sid, ids_all, 2, ENDPOINT_GEMINI_INTERACTIONS, 0);
    assert(json != NULL);
    assert(strstr(json, "\"type\":\"user_input\"") != NULL);
    assert(strstr(json, "\"type\":\"model_output\"") != NULL);
    free(json);

    /* Simulate storing interaction state: set last_synced_entry_id = id2 */
    sqlite3_exec(db, "UPDATE sessions SET last_synced_entry_id=2 WHERE id=1;", NULL, NULL, NULL);

    /* Second turn: new user + tool_call + tool_result */
    int64_t id3 = entry_append_typed(db, sid, 3, "user_message", 0, "Do it", NULL, NULL, 0, STOP_REASON_NONE, NULL, 0, 0, 0);
    int64_t id4 = entry_append_typed(db, sid, 4, "tool_call", 0, "{\"x\":1}", "call_i1", "test", 0, STOP_REASON_NONE, NULL, 0, 0, 0);
    int64_t id5 = entry_append_typed(db, sid, 4, "tool_result", 1, "done", "call_i1", "test", 0, STOP_REASON_NONE, NULL, 0, 0, 0);

    /* Delta only (entries after id2) */
    int64_t ids_delta[] = {id1, id2, id3, id4, id5};
    json = db_build_request_typed(db, sid, ids_delta, 5, ENDPOINT_GEMINI_INTERACTIONS, 0);
    assert(json != NULL);
    /* Should NOT contain the first turn entries */
    assert(strstr(json, "Hello") == NULL);
    assert(strstr(json, "Hi there") == NULL);
    /* Should contain the new entries */
    assert(strstr(json, "\"type\":\"user_input\"") != NULL);
    assert(strstr(json, "Do it") != NULL);
    assert(strstr(json, "\"type\":\"function_call\"") != NULL);
    assert(strstr(json, "\"type\":\"function_result\"") != NULL);
    free(json);

    db_close(db);
    printf("  gemini_interactions_egress... PASS\n");
}

int main(void) {
    printf("test_typed_entries:\n");
    test_insert_typed_entries();
    test_tool_result_typed();
    test_legacy_entry_gets_type();
    test_tool_calls_result_entry_id();
    test_ingest_typed();
    test_openai_egress();
    test_responses_egress();
    test_gemini_egress();
    test_gemini_interactions_egress();
    printf("\n9/9 tests passed.\n");
    return 0;
}
