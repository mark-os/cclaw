#include "db.h"
#include "test_util.h"
#include <assert.h>

/* Smoke: one iteration of entries (user, assistant with tool calls, tool
 * result) round-trips through the branch, and iteration ids advance. */
static void test_entries_roundtrip(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "t", NULL, -1, 0);
    assert(sid > 0);
    int64_t it = db_next_iteration_id(db, sid);
    assert(it == 1);

    Message m1 = {.role = ROLE_USER, .content = "go"};
    entry_append_with_iteration(db, sid, &m1, it);
    ToolCall tcs[2] = {
        {.id = "c1", .name = "shell_exec", .arguments = "{\"cmd\":\"ls\"}"},
        {.id = "c2", .name = "js_eval", .arguments = "{\"code\":\"1+1\"}"},
    };
    Message m2 = {.role = ROLE_ASSISTANT, .tool_calls = tcs, .tool_call_count = 2,
                  .stop_reason = STOP_REASON_LENGTH};
    entry_append_with_iteration(db, sid, &m2, it);
    ToolResult tr = {.tool_call_id = "c1", .content = "output"};
    Message m3 = {.role = ROLE_TOOL, .tool_result = &tr, .tool_name = "shell_exec"};
    entry_append_with_iteration(db, sid, &m3, it);
    assert(db_next_iteration_id(db, sid) == it + 1);

    int count = 0;
    Entry *b = session_get_branch(db, sid, &count);
    assert(count == 3);
    assert(b[1].message.stop_reason == STOP_REASON_LENGTH);
    assert(b[1].message.tool_call_count == 2);
    assert(strcmp(b[1].message.tool_calls[0].id, "c1") == 0);
    assert(strcmp(b[1].message.tool_calls[1].arguments, "{\"code\":\"1+1\"}") == 0);
    assert(b[2].message.role == ROLE_TOOL);
    assert(strcmp(b[2].message.tool_result->content, "output") == 0);
    entry_branch_free(b, count);
    assert(test_qi(db, "SELECT COUNT(*) FROM entries WHERE iteration_id=1") == 3);
    db_close(db);
}

/* The sub-agent concurrency cap counts running children and children that
 * were just launched (idle with a queued task); a drained child frees it. */
static void test_session_count_active_agents(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t parent = session_create(db, "parent", NULL, -1, 0);
    int64_t s1 = session_create(db, "a", NULL, parent, 1);
    int64_t s2 = session_create(db, "b", NULL, parent, 1);
    session_create(db, "c", NULL, parent, 1);

    assert(session_count_active_agents(db) == 0);
    session_set_state(db, s1, "llm_running");
    assert(session_count_active_agents(db) == 1);
    session_set_state(db, s2, "llm_running");
    assert(session_count_active_agents(db) == 2);
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
}

int main(void) {
    TEST_INIT();
    RUN(test_entries_roundtrip);
    RUN(test_session_count_active_agents);
    return 0;
}
