/* T93/V17/V30: Agent crash (simulated SIGKILL) → next fork recovers via
 * incomplete turn notice. Verifies the full DB-backed path:
 * 1. Session has assistant entry with tool_calls but no tool_results (crash)
 * 2. session_get_branch loads the incomplete state
 * 3. context_build synthesizes missing tool_results + system notice */
#include "db.h"
#include "context.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *DB_PATH = "/tmp/test_cclaw_crash_recovery.sqlite";

/* Simulate crash: write user msg + assistant with tool_calls, but NO tool_results.
 * This is exactly what V30 describes — agent got SIGKILL after writing the
 * assistant entry but before any tool execution completed. */
static void test_crash_recovery_synthesizes_notice(void) {
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    assert(db);

    int64_t sid = session_create(db, "crash_test", NULL, -1, 0);
    assert(sid > 0);

    /* User message */
    Message user_msg = {.role = ROLE_USER, .content = "list files"};
    int64_t uid = entry_append(db, sid, &user_msg);
    assert(uid > 0);

    /* Assistant with tool_calls — simulates the entry written before crash.
     * We need to write raw JSON to DB since entry_append doesn't handle tool_calls
     * in the simplified Message struct for writes. Insert directly. */
    const char *asst_json =
        "{\"type\":\"message\",\"role\":\"assistant\","
        "\"content\":[{\"type\":\"tool_call\",\"id\":\"call_1\","
        "\"name\":\"shell_exec\",\"arguments\":\"{\\\"cmd\\\":\\\"ls\\\"}\"},"
        "{\"type\":\"tool_call\",\"id\":\"call_2\","
        "\"name\":\"file_read\",\"arguments\":\"{\\\"path\\\":\\\"README.md\\\"}\"}]}";

    int64_t turn_id = db_next_turn_id(db, sid);
    const char *sql =
        "INSERT INTO entries(session_id, parent_id, turn_id, data) VALUES(?,?,?,?);";
    sqlite3_stmt *stmt;
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, sid);
    sqlite3_bind_int64(stmt, 2, uid);
    sqlite3_bind_int64(stmt, 3, turn_id);
    sqlite3_bind_text(stmt, 4, asst_json, -1, SQLITE_STATIC);
    assert(sqlite3_step(stmt) == SQLITE_DONE);
    int64_t asst_id = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(stmt);

    /* Update session leaf to point to assistant entry */
    session_set_leaf(db, sid, asst_id);

    /* --- Crash happened here: no tool_results written --- */

    /* Now simulate next fork: load branch from DB */
    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    assert(branch);
    assert(count == 2); /* user + assistant */

    /* Verify assistant has tool_calls loaded */
    assert(branch[1].message.role == ROLE_ASSISTANT);
    assert(branch[1].message.tool_calls != NULL);
    assert(branch[1].message.tool_call_count == 2);

    /* Run context_build — should detect incomplete turn (V17) */
    Config cfg = {0};
    cfg.provider.context_window = 128000;

    Message *msgs = NULL;
    int msg_count = 0;
    int rc = context_build(branch, count, &cfg, &msgs, &msg_count);
    assert(rc == 0);

    /* Expected: user + assistant + 2 synthetic tool_results + system notice = 5 */
    assert(msg_count == 5);

    /* Messages 0,1 = user, assistant */
    assert(msgs[0].role == ROLE_USER);
    assert(msgs[1].role == ROLE_ASSISTANT);

    /* Messages 2,3 = synthetic tool_results for call_1 and call_2 */
    assert(msgs[2].role == ROLE_TOOL);
    assert(msgs[2].tool_result != NULL);
    assert(strcmp(msgs[2].tool_result->tool_call_id, "call_1") == 0);
    assert(strstr(msgs[2].tool_result->content, "terminated") != NULL);

    assert(msgs[3].role == ROLE_TOOL);
    assert(msgs[3].tool_result != NULL);
    assert(strcmp(msgs[3].tool_result->tool_call_id, "call_2") == 0);
    assert(strstr(msgs[3].tool_result->content, "terminated") != NULL);

    /* Message 4 = system notice about interruption */
    assert(msgs[4].role == ROLE_SYSTEM);
    assert(strstr(msgs[4].content, "interrupted") != NULL);

    context_free(msgs, msg_count);
    entry_branch_free(branch, count);
    db_close(db);
    unlink(DB_PATH);
    printf("  PASS test_crash_recovery_synthesizes_notice\n");
}

/* Verify that a partial crash (1 of 2 tool_results written) also recovers */
static void test_crash_recovery_partial_results(void) {
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    assert(db);

    int64_t sid = session_create(db, "partial_crash", NULL, -1, 0);
    assert(sid > 0);

    Message user_msg = {.role = ROLE_USER, .content = "do two things"};
    int64_t uid = entry_append(db, sid, &user_msg);

    /* Assistant with 2 tool_calls */
    const char *asst_json =
        "{\"type\":\"message\",\"role\":\"assistant\","
        "\"content\":[{\"type\":\"tool_call\",\"id\":\"tc_a\","
        "\"name\":\"shell_exec\",\"arguments\":\"{\\\"cmd\\\":\\\"echo hi\\\"}\"},"
        "{\"type\":\"tool_call\",\"id\":\"tc_b\","
        "\"name\":\"file_read\",\"arguments\":\"{\\\"path\\\":\\\"x\\\"}\"}]}";

    int64_t turn_id = db_next_turn_id(db, sid);
    const char *sql =
        "INSERT INTO entries(session_id, parent_id, turn_id, data) VALUES(?,?,?,?);";
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, sid);
    sqlite3_bind_int64(stmt, 2, uid);
    sqlite3_bind_int64(stmt, 3, turn_id);
    sqlite3_bind_text(stmt, 4, asst_json, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    int64_t asst_id = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(stmt);

    /* One tool_result was written before crash */
    const char *tr_json =
        "{\"type\":\"message\",\"role\":\"tool_result\","
        "\"tool_call_id\":\"tc_a\",\"name\":\"shell_exec\",\"content\":\"hi\"}";
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, sid);
    sqlite3_bind_int64(stmt, 2, asst_id);
    sqlite3_bind_int64(stmt, 3, turn_id);
    sqlite3_bind_text(stmt, 4, tr_json, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    int64_t tr_id = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(stmt);

    session_set_leaf(db, sid, tr_id);

    /* Load branch and run context_build */
    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    assert(branch);
    assert(count == 3); /* user + assistant + 1 tool_result */

    Config cfg = {0};
    cfg.provider.context_window = 128000;

    Message *msgs = NULL;
    int msg_count = 0;
    int rc = context_build(branch, count, &cfg, &msgs, &msg_count);
    assert(rc == 0);

    /* Expected: user + assistant + existing tool_result + 1 synthetic + notice = 5 */
    assert(msg_count == 5);
    assert(msgs[2].role == ROLE_TOOL);
    assert(msgs[2].tool_result && strcmp(msgs[2].tool_result->tool_call_id, "tc_a") == 0);

    /* Synthetic for tc_b */
    assert(msgs[3].role == ROLE_TOOL);
    assert(msgs[3].tool_result && strcmp(msgs[3].tool_result->tool_call_id, "tc_b") == 0);
    assert(strstr(msgs[3].tool_result->content, "terminated") != NULL);

    assert(msgs[4].role == ROLE_SYSTEM);
    assert(strstr(msgs[4].content, "interrupted") != NULL);

    context_free(msgs, msg_count);
    entry_branch_free(branch, count);
    db_close(db);
    unlink(DB_PATH);
    printf("  PASS test_crash_recovery_partial_results\n");
}

/* Verify that a complete turn (no crash) does NOT trigger recovery */
static void test_no_crash_no_recovery(void) {
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    assert(db);

    int64_t sid = session_create(db, "no_crash", NULL, -1, 0);
    int64_t uid = entry_append(db, sid, &(Message){.role = ROLE_USER, .content = "hi"});

    /* Assistant with 1 tool_call */
    const char *asst_json =
        "{\"type\":\"message\",\"role\":\"assistant\","
        "\"content\":[{\"type\":\"tool_call\",\"id\":\"tc_x\","
        "\"name\":\"shell_exec\",\"arguments\":\"{\\\"cmd\\\":\\\"pwd\\\"}\"}]}";

    int64_t turn_id = db_next_turn_id(db, sid);
    const char *sql =
        "INSERT INTO entries(session_id, parent_id, turn_id, data) VALUES(?,?,?,?);";
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, sid);
    sqlite3_bind_int64(stmt, 2, uid);
    sqlite3_bind_int64(stmt, 3, turn_id);
    sqlite3_bind_text(stmt, 4, asst_json, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    int64_t asst_id = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(stmt);

    /* Tool result present — no crash */
    const char *tr_json =
        "{\"type\":\"message\",\"role\":\"tool_result\","
        "\"tool_call_id\":\"tc_x\",\"name\":\"shell_exec\",\"content\":\"/home\"}";
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, sid);
    sqlite3_bind_int64(stmt, 2, asst_id);
    sqlite3_bind_int64(stmt, 3, turn_id);
    sqlite3_bind_text(stmt, 4, tr_json, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    int64_t tr_id = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(stmt);

    session_set_leaf(db, sid, tr_id);

    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    assert(count == 3);

    Config cfg = {0};
    cfg.provider.context_window = 128000;

    Message *msgs = NULL;
    int msg_count = 0;
    int rc = context_build(branch, count, &cfg, &msgs, &msg_count);
    assert(rc == 0);

    /* No synthesis — just user + assistant + tool_result = 3 */
    assert(msg_count == 3);
    assert(msgs[0].role == ROLE_USER);
    assert(msgs[1].role == ROLE_ASSISTANT);
    assert(msgs[2].role == ROLE_TOOL);

    context_free(msgs, msg_count);
    entry_branch_free(branch, count);
    db_close(db);
    unlink(DB_PATH);
    printf("  PASS test_no_crash_no_recovery\n");
}

int main(void) {
    printf("test_crash_recovery:\n");
    test_crash_recovery_synthesizes_notice();
    test_crash_recovery_partial_results();
    test_no_crash_no_recovery();
    printf("All crash recovery tests passed.\n");
    return 0;
}
