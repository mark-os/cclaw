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

/* Helper: insert assistant entry with tool_calls using split columns */
static int64_t insert_assistant_with_tools(sqlite3 *db, int64_t session_id,
                                           int64_t parent_id, int64_t turn_id,
                                           const char *tool_calls_json, int tc_count) {
    const char *sql =
        "INSERT INTO entries(session_id, parent_id, turn_id, role, tool_calls, tool_call_count,"
        " stop_reason, token_estimate, content_bytes)"
        " VALUES(?,?,?,2,?,?,3,10,0);";
    sqlite3_stmt *stmt;
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_int64(stmt, 2, parent_id);
    sqlite3_bind_int64(stmt, 3, turn_id);
    sqlite3_bind_text(stmt, 4, tool_calls_json, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 5, tc_count);
    assert(sqlite3_step(stmt) == SQLITE_DONE);
    int64_t id = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(stmt);
    return id;
}

/* Helper: insert tool_result entry using split columns */
static int64_t insert_tool_result(sqlite3 *db, int64_t session_id,
                                  int64_t parent_id, int64_t turn_id,
                                  const char *tool_call_id, const char *content) {
    const char *sql =
        "INSERT INTO entries(session_id, parent_id, turn_id, role, content, tool_call_id,"
        " token_estimate, content_bytes)"
        " VALUES(?,?,?,3,?,?,?,?);";
    sqlite3_stmt *stmt;
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_int64(stmt, 2, parent_id);
    sqlite3_bind_int64(stmt, 3, turn_id);
    sqlite3_bind_text(stmt, 4, content, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, tool_call_id, -1, SQLITE_STATIC);
    int clen = content ? (int)strlen(content) : 0;
    sqlite3_bind_int(stmt, 6, (clen / 4) + 4);
    sqlite3_bind_int(stmt, 7, clen);
    assert(sqlite3_step(stmt) == SQLITE_DONE);
    int64_t id = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(stmt);
    return id;
}

static void test_crash_recovery_synthesizes_notice(void) {
    unlink(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);

    int64_t sid = session_create(db, "crash_test", NULL, -1, 0);
    assert(sid > 0);

    /* User message */
    Message user_msg = {.role = ROLE_USER, .content = "list files"};
    int64_t uid = entry_append(db, sid, &user_msg);
    assert(uid > 0);

    /* Assistant with tool_calls — simulates the entry written before crash */
    const char *tc_json =
        "[{\"id\":\"call_1\",\"name\":\"shell_exec\",\"args\":{\"cmd\":\"ls\"}},"
        "{\"id\":\"call_2\",\"name\":\"file_read\",\"args\":{\"path\":\"README.md\"}}]";

    int64_t turn_id = db_next_turn_id(db, sid);
    int64_t asst_id = insert_assistant_with_tools(db, sid, uid, turn_id, tc_json, 2);
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

    assert(msgs[0].role == ROLE_USER);
    assert(msgs[1].role == ROLE_ASSISTANT);

    assert(msgs[2].role == ROLE_TOOL);
    assert(msgs[2].tool_result != NULL);
    assert(strcmp(msgs[2].tool_result->tool_call_id, "call_1") == 0);
    assert(strstr(msgs[2].tool_result->content, "terminated") != NULL);

    assert(msgs[3].role == ROLE_TOOL);
    assert(msgs[3].tool_result != NULL);
    assert(strcmp(msgs[3].tool_result->tool_call_id, "call_2") == 0);
    assert(strstr(msgs[3].tool_result->content, "terminated") != NULL);

    assert(msgs[4].role == ROLE_SYSTEM);
    assert(strstr(msgs[4].content, "interrupted") != NULL);

    context_free(msgs, msg_count);
    entry_branch_free(branch, count);
    db_close(db);
    unlink(DB_PATH);
    printf("  PASS test_crash_recovery_synthesizes_notice\n");
}

static void test_crash_recovery_partial_results(void) {
    unlink(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);

    int64_t sid = session_create(db, "partial_crash", NULL, -1, 0);
    assert(sid > 0);

    Message user_msg = {.role = ROLE_USER, .content = "do two things"};
    int64_t uid = entry_append(db, sid, &user_msg);

    /* Assistant with 2 tool_calls */
    const char *tc_json =
        "[{\"id\":\"tc_a\",\"name\":\"shell_exec\",\"args\":{\"cmd\":\"echo hi\"}},"
        "{\"id\":\"tc_b\",\"name\":\"file_read\",\"args\":{\"path\":\"x\"}}]";

    int64_t turn_id = db_next_turn_id(db, sid);
    int64_t asst_id = insert_assistant_with_tools(db, sid, uid, turn_id, tc_json, 2);

    /* One tool_result was written before crash */
    int64_t tr_id = insert_tool_result(db, sid, asst_id, turn_id, "tc_a", "hi");
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

static void test_no_crash_no_recovery(void) {
    unlink(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);

    int64_t sid = session_create(db, "no_crash", NULL, -1, 0);
    int64_t uid = entry_append(db, sid, &(Message){.role = ROLE_USER, .content = "hi"});

    /* Assistant with 1 tool_call */
    const char *tc_json =
        "[{\"id\":\"tc_x\",\"name\":\"shell_exec\",\"args\":{\"cmd\":\"pwd\"}}]";

    int64_t turn_id = db_next_turn_id(db, sid);
    int64_t asst_id = insert_assistant_with_tools(db, sid, uid, turn_id, tc_json, 1);

    /* Tool result present — no crash */
    int64_t tr_id = insert_tool_result(db, sid, asst_id, turn_id, "tc_x", "/home");
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
