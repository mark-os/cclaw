#include "db.h"
#include "config_registry.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static void test_next_iteration_id_empty(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "t", NULL, -1, 0);
    assert(sid > 0);
    /* Empty session → iteration_id should be 1 */
    int64_t tid = db_next_iteration_id(db, sid);
    assert(tid == 1);
    db_close(db);
    printf("  PASS test_next_iteration_id_empty\n");
}

static void test_next_iteration_id_increments(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "t", NULL, -1, 0);

    Message m = {.role = ROLE_USER, .content = "hi"};
    int64_t tid1 = db_next_iteration_id(db, sid);
    entry_append_with_iteration(db, sid, &m, tid1);

    int64_t tid2 = db_next_iteration_id(db, sid);
    assert(tid2 == tid1 + 1);

    db_close(db);
    printf("  PASS test_next_iteration_id_increments\n");
}

static void test_entry_append_stores_iteration_id(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "t", NULL, -1, 0);

    Message m1 = {.role = ROLE_USER, .content = "hello"};
    Message m2 = {.role = ROLE_ASSISTANT, .content = "hi"};
    int64_t tid = 42;
    entry_append_with_iteration(db, sid, &m1, tid);
    entry_append_with_iteration(db, sid, &m2, tid);

    /* Verify both entries share the same iteration_id */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT iteration_id FROM entries WHERE session_id=? ORDER BY id", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, sid);
    int row = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        assert(sqlite3_column_int64(stmt, 0) == 42);
        row++;
    }
    sqlite3_finalize(stmt);
    assert(row == 2);

    db_close(db);
    printf("  PASS test_entry_append_stores_iteration_id\n");
}

static void test_session_get_agent_name(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    test_seed_agent(db, "myagent");  /* sessions.agent_name FK (v31) */
    int64_t sid = session_create(db, "t", "myagent", -1, 0);
    char *name = session_get_agent_name(db, sid);
    assert(name != NULL);
    assert(strcmp(name, "myagent") == 0);
    free(name);

    /* NULL agent_name */
    int64_t sid2 = session_create(db, "t2", NULL, -1, 0);
    char *name2 = session_get_agent_name(db, sid2);
    assert(name2 == NULL);

    db_close(db);
    printf("  PASS test_session_get_agent_name\n");
}

static void test_session_get_depth(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "top", NULL, -1, 0);
    assert(session_get_depth(db, sid) == 0);

    int64_t child = session_create(db, "child", NULL, sid, 1);
    assert(session_get_depth(db, child) == 1);

    db_close(db);
    printf("  PASS test_session_get_depth\n");
}

static void test_session_count_active_agents(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t parent = session_create(db, "parent", NULL, -1, 0);
    /* Sub-agents have parent_session_id > 0 */
    int64_t s1 = session_create(db, "a", NULL, parent, 1);
    int64_t s2 = session_create(db, "b", NULL, parent, 1);
    session_create(db, "c", NULL, parent, 1);

    /* All idle initially */
    assert(session_count_active_agents(db) == 0);

    /* Set some to running */
    session_set_state(db, s1, "llm_running");
    assert(session_count_active_agents(db) == 1);

    session_set_state(db, s2, "llm_running");
    assert(session_count_active_agents(db) == 2);

    /* Back to idle */
    session_set_state(db, s1, "idle");
    assert(session_count_active_agents(db) == 1);

    /* Idle with a queued task = just launched, not yet advanced — counted,
     * so a batch of launches counts against its own cap. Consuming the row
     * (a finished child) frees the slot. */
    int64_t s4 = session_create(db, "d", NULL, parent, 1);
    inbox_insert(db, s4, "spawn", NULL, "task");
    assert(session_count_active_agents(db) == 2);
    assert(sqlite3_exec(db, "UPDATE inbox SET consumed=1;", NULL, NULL, NULL)
           == SQLITE_OK);
    assert(session_count_active_agents(db) == 1);

    db_close(db);
    printf("  PASS test_session_count_active_agents\n");
}

static void test_entry_append_stop_reason_roundtrip(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "t", NULL, -1, 0);

    Message m = {.role = ROLE_ASSISTANT, .content = "done",
                 .stop_reason = STOP_REASON_LENGTH};
    entry_append_with_iteration(db, sid, &m, 1);

    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    assert(count == 1);
    assert(branch[0].message.stop_reason == STOP_REASON_LENGTH);
    entry_branch_free(branch, count);

    db_close(db);
    printf("  PASS test_entry_append_stop_reason_roundtrip\n");
}

static void test_entry_append_multiple_tool_calls(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "t", NULL, -1, 0);

    Message m1 = {.role = ROLE_USER, .content = "go"};
    entry_append_with_iteration(db, sid, &m1, 1);

    ToolCall tcs[3] = {
        {.id = "c1", .name = "shell_exec", .arguments = "{\"cmd\":\"ls\"}"},
        {.id = "c2", .name = "file_read", .arguments = "{\"path\":\"x.txt\"}"},
        {.id = "c3", .name = "js_eval", .arguments = "{\"code\":\"1+1\"}"},
    };
    Message m2 = {.role = ROLE_ASSISTANT, .content = NULL,
                  .tool_calls = tcs, .tool_call_count = 3};
    entry_append_with_iteration(db, sid, &m2, 1);

    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    assert(count == 2);
    assert(branch[1].message.tool_call_count == 3);
    assert(strcmp(branch[1].message.tool_calls[0].id, "c1") == 0);
    assert(strcmp(branch[1].message.tool_calls[1].name, "file_read") == 0);
    assert(strcmp(branch[1].message.tool_calls[2].arguments, "{\"code\":\"1+1\"}") == 0);
    entry_branch_free(branch, count);

    db_close(db);
    printf("  PASS test_entry_append_multiple_tool_calls\n");
}

static void test_entry_append_tool_result_with_name(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "t", NULL, -1, 0);

    Message m1 = {.role = ROLE_USER, .content = "go"};
    entry_append_with_iteration(db, sid, &m1, 1);

    ToolCall tc = {.id = "c1", .name = "shell_exec", .arguments = "{}"};
    Message m2 = {.role = ROLE_ASSISTANT, .tool_calls = &tc, .tool_call_count = 1};
    entry_append_with_iteration(db, sid, &m2, 1);

    ToolResult tr = {.tool_call_id = "c1", .content = "output"};
    Message m3 = {.role = ROLE_TOOL, .tool_result = &tr,
                  .tool_name = "shell_exec", .is_error = 0};
    entry_append_with_iteration(db, sid, &m3, 1);

    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    assert(count == 3);
    assert(branch[2].message.role == ROLE_TOOL);
    assert(branch[2].message.tool_result != NULL);
    assert(strcmp(branch[2].message.tool_result->content, "output") == 0);
    entry_branch_free(branch, count);

    db_close(db);
    printf("  PASS test_entry_append_tool_result_with_name\n");
}

static void test_session_state_waiting_to_idle(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "t", NULL, -1, 0);

    assert(session_set_state(db, sid, "llm_running") == 0);
    assert(session_set_state(db, sid, "idle") == 0);
    assert(session_set_state(db, sid, "tool_running") == 0);
    assert(session_set_state(db, sid, "idle") == 0);

    db_close(db);
    printf("  PASS test_session_state_waiting_to_idle\n");
}

static void test_config_get_nonexistent(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    /* Unregistered key with no row returns NULL */
    char *val = config_get(db, "nonexistent_key");
    assert(val == NULL);
    db_close(db);
    printf("  PASS test_config_get_nonexistent\n");
}

static void test_config_set_overwrite(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    /* Set a registered key twice — keeps last value */
    config_set(db, "max_iterations", "50");
    config_set(db, "max_iterations", "99");
    assert(config_get_int(db, "max_iterations") == 99);
    db_close(db);
    printf("  PASS test_config_set_overwrite\n");
}

int main(void) {
    TEST_INIT();
    alarm(10);
    printf("test_db_edge:\n");
    test_next_iteration_id_empty();
    test_next_iteration_id_increments();
    test_entry_append_stores_iteration_id();
    test_session_get_agent_name();
    test_session_get_depth();
    test_session_count_active_agents();
    test_entry_append_stop_reason_roundtrip();
    test_entry_append_multiple_tool_calls();
    test_entry_append_tool_result_with_name();
    test_session_state_waiting_to_idle();
    test_config_get_nonexistent();
    test_config_set_overwrite();
    printf("All db_edge tests passed.\n");
    return 0;
}
