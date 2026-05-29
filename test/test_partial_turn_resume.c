/* T224/V87: Partial-turn resume — on agent startup, detect incomplete
 * tool_call batch and execute remaining calls. Tests:
 * 1. PENDING entry → replaced with error
 * 2. Missing results (no PENDING) → dispatched
 * 3. All results present → no-op */
#include "agent.h"
#include "db.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *DB_PATH = "/tmp/test_partial_turn_resume.sqlite";

/* Track which tools were dispatched */
static int dispatch_count;
static char dispatched_names[10][64];

static char *mock_dispatch(const char *name, const char *arguments, void *user_data) {
    (void)arguments; (void)user_data;
    if (dispatch_count < 10)
        snprintf(dispatched_names[dispatch_count], 64, "%s", name);
    dispatch_count++;
    char *r = malloc(64);
    snprintf(r, 64, "ok from %s", name);
    return r;
}

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

/* Test: PENDING entry gets replaced with error */
static void test_pending_replaced_with_error(void) {
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    assert(db);

    int64_t sid = session_create(db, "pending_test", NULL, -1, 0);
    Message user_msg = {.role = ROLE_USER, .content = "do stuff"};
    int64_t uid = entry_append(db, sid, &user_msg);

    const char *tc_json =
        "[{\"id\":\"call_1\",\"name\":\"shell_exec\",\"args\":{\"cmd\":\"ls\"}},"
        "{\"id\":\"call_2\",\"name\":\"file_read\",\"args\":{\"path\":\"x\"}}]";
    int64_t turn_id = db_next_turn_id(db, sid);
    int64_t asst_id = insert_assistant_with_tools(db, sid, uid, turn_id, tc_json, 2);

    /* First result resolved, second is PENDING */
    int64_t tr1 = insert_tool_result(db, sid, asst_id, turn_id, "call_1", "file list");
    int64_t tr2 = insert_tool_result(db, sid, tr1, turn_id, "call_2", "PENDING");
    session_set_leaf(db, sid, tr2);

    dispatch_count = 0;
    Config cfg = {0};
    cfg.provider.context_window = 128000;
    cfg.provider.base_url = "http://localhost:1";
    cfg.provider.api_key = "test";
    cfg.provider.model = "test";
    cfg.max_iterations = 1;

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;

    /* agent_run will call partial_turn_resume first */
    /* Since there's a PENDING, it should replace with error and proceed to LLM loop.
     * LLM loop will fail (no real server), but we verify the PENDING was replaced. */
    agent_run(&ctx);

    /* Verify PENDING was replaced */
    const char *check_sql = "SELECT content FROM entries WHERE session_id=? AND tool_call_id='call_2';";
    sqlite3_stmt *stmt;
    assert(sqlite3_prepare_v2(db, check_sql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, sid);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    const char *content = (const char *)sqlite3_column_text(stmt, 0);
    assert(strstr(content, "daemon did not deliver") != NULL);
    sqlite3_finalize(stmt);

    /* No tools should have been dispatched for the resume (PENDING → error, not re-execute) */
    assert(dispatch_count == 0);

    db_close(db);
    unlink(DB_PATH);
    printf("  PASS test_pending_replaced_with_error\n");
}

/* Test: Missing results (daemon resolved PENDING, remaining unexecuted) → dispatch */
static void test_missing_results_dispatched(void) {
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    assert(db);

    int64_t sid = session_create(db, "resume_test", NULL, -1, 0);
    Message user_msg = {.role = ROLE_USER, .content = "do three things"};
    int64_t uid = entry_append(db, sid, &user_msg);

    const char *tc_json =
        "[{\"id\":\"c1\",\"name\":\"shell_exec\",\"args\":{\"cmd\":\"echo 1\"}},"
        "{\"id\":\"c2\",\"name\":\"file_read\",\"args\":{\"path\":\"a\"}},"
        "{\"id\":\"c3\",\"name\":\"file_write\",\"args\":{\"path\":\"b\",\"content\":\"x\"}}]";
    int64_t turn_id = db_next_turn_id(db, sid);
    int64_t asst_id = insert_assistant_with_tools(db, sid, uid, turn_id, tc_json, 3);

    /* Only first result present (daemon resolved the PENDING for c1, remaining unexecuted) */
    int64_t tr1 = insert_tool_result(db, sid, asst_id, turn_id, "c1", "done");
    session_set_leaf(db, sid, tr1);

    dispatch_count = 0;
    Config cfg = {0};
    cfg.provider.context_window = 128000;
    cfg.provider.base_url = "http://localhost:1";
    cfg.provider.api_key = "test";
    cfg.provider.model = "test";
    cfg.max_iterations = 1;

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;

    /* agent_run calls partial_turn_resume → dispatches c2 and c3 */
    agent_run(&ctx);

    /* Verify c2 and c3 were dispatched */
    assert(dispatch_count == 2);
    assert(strcmp(dispatched_names[0], "file_read") == 0);
    assert(strcmp(dispatched_names[1], "file_write") == 0);

    /* Verify results written to DB */
    const char *check_sql = "SELECT content FROM entries WHERE session_id=? AND tool_call_id=?;";
    sqlite3_stmt *stmt;
    assert(sqlite3_prepare_v2(db, check_sql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, sid);
    sqlite3_bind_text(stmt, 2, "c2", -1, SQLITE_STATIC);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strstr((const char *)sqlite3_column_text(stmt, 0), "ok from file_read") != NULL);
    sqlite3_finalize(stmt);

    assert(sqlite3_prepare_v2(db, check_sql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, sid);
    sqlite3_bind_text(stmt, 2, "c3", -1, SQLITE_STATIC);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strstr((const char *)sqlite3_column_text(stmt, 0), "ok from file_write") != NULL);
    sqlite3_finalize(stmt);

    db_close(db);
    unlink(DB_PATH);
    printf("  PASS test_missing_results_dispatched\n");
}

/* Test: All results present → no dispatch, normal flow */
static void test_all_results_present_noop(void) {
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    assert(db);

    int64_t sid = session_create(db, "complete_test", NULL, -1, 0);
    Message user_msg = {.role = ROLE_USER, .content = "do it"};
    int64_t uid = entry_append(db, sid, &user_msg);

    const char *tc_json =
        "[{\"id\":\"x1\",\"name\":\"shell_exec\",\"args\":{\"cmd\":\"pwd\"}}]";
    int64_t turn_id = db_next_turn_id(db, sid);
    int64_t asst_id = insert_assistant_with_tools(db, sid, uid, turn_id, tc_json, 1);

    int64_t tr1 = insert_tool_result(db, sid, asst_id, turn_id, "x1", "/home");
    session_set_leaf(db, sid, tr1);

    dispatch_count = 0;
    Config cfg = {0};
    cfg.provider.context_window = 128000;
    cfg.provider.base_url = "http://localhost:1";
    cfg.provider.api_key = "test";
    cfg.provider.model = "test";
    cfg.max_iterations = 1;

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;

    agent_run(&ctx);

    /* No tools dispatched during resume (all results already present) */
    assert(dispatch_count == 0);

    db_close(db);
    unlink(DB_PATH);
    printf("  PASS test_all_results_present_noop\n");
}

int main(void) {
    alarm(10);
    printf("test_partial_turn_resume:\n");
    test_pending_replaced_with_error();
    test_missing_results_dispatched();
    test_all_results_present_noop();
    printf("All partial-turn resume tests passed.\n");
    return 0;
}
