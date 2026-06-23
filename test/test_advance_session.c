/* Test advance_session decision logic */
#include "advance.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_idle_no_inbox(void) {
    sqlite3 *db = test_db_open(":memory:");
    int64_t sid = session_create(db, "test", "default", -1, 0);
    assert(sid > 0);

    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_NOOP);
    assert(out.session_id == sid);

    db_close(db);
    printf("  PASS test_idle_no_inbox\n");
}

static void test_idle_with_inbox(void) {
    sqlite3 *db = test_db_open(":memory:");
    int64_t sid = session_create(db, "test", "default", -1, 0);
    inbox_insert(db, sid, "cli", "hello");

    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);
    assert(out.iteration == 0);
    assert(strcmp(out.agent_name, "default") == 0);
    /* Inbox should be consumed */
    assert(inbox_count(db, sid) == 0);
    /* Iteration should be 0 */
    assert(session_get_iteration(db, sid) == 0);

    db_close(db);
    printf("  PASS test_idle_with_inbox\n");
}

static void test_llm_complete_stop(void) {
    sqlite3 *db = test_db_open(":memory:");
    int64_t sid = session_create(db, "test", "default", -1, 0);

    /* Simulate: session in llm_running, LLM wrote a stop entry */
    session_set_state(db, sid, "llm_running");
    Message msg = { .role = ROLE_ASSISTANT, .content = "done",
                    .stop_reason = STOP_REASON_STOP };
    int64_t turn_id = db_next_turn_id(db, sid);
    entry_append_with_turn(db, sid, &msg, turn_id);

    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_DONE);

    db_close(db);
    printf("  PASS test_llm_complete_stop\n");
}

static void test_tool_running_all_done(void) {
    sqlite3 *db = test_db_open(":memory:");
    int64_t sid = session_create(db, "test", "default", -1, 0);

    /* Session in tool_running, no pending tool_calls, iteration 2 */
    session_set_state(db, sid, "llm_running");
    session_set_state(db, sid, "tool_running");
    session_set_iteration(db, sid, 2);

    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);
    assert(out.iteration == 3); /* bumped from 2 */
    assert(session_get_iteration(db, sid) == 3);

    db_close(db);
    printf("  PASS test_tool_running_all_done\n");
}

static void test_max_iterations(void) {
    sqlite3 *db = test_db_open(":memory:");
    int64_t sid = session_create(db, "test", "default", -1, 0);

    /* Session at iteration 24 (max=25), tool_running with no pending calls */
    session_set_state(db, sid, "llm_running");
    session_set_state(db, sid, "tool_running");
    session_set_iteration(db, sid, 24);

    AdvanceOutput out = advance_session(db, sid, 25);
    /* bump to 25 >= max → error + done */
    assert(out.action == ADVANCE_DONE);

    db_close(db);
    printf("  PASS test_max_iterations\n");
}

static void test_waiting(void) {
    sqlite3 *db = test_db_open(":memory:");
    int64_t sid = session_create(db, "test", "default", -1, 0);
    session_set_state(db, sid, "llm_running");
    session_set_state(db, sid, "tool_running");

    /* An async tool_call still in flight (status='running' — e.g. a sub-agent
     * or a forked tool) keeps the turn parked in tool_running: advance must
     * WAIT, not bump to the LLM, until every call has a result. */
    char ins[256];
    snprintf(ins, sizeof(ins),
        "INSERT INTO tool_calls(session_id, entry_id, call_id, name, status)"
        " VALUES(%lld, 1, 'c1', 'launch_agent', 'running');", (long long)sid);
    assert(sqlite3_exec(db, ins, NULL, NULL, NULL) == SQLITE_OK);

    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_WAITING);

    db_close(db);
    printf("  PASS test_waiting\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_advance_session:\n");
    test_idle_no_inbox();
    test_idle_with_inbox();
    test_llm_complete_stop();
    test_tool_running_all_done();
    test_max_iterations();
    test_waiting();
    printf("All advance_session tests passed.\n");
    return 0;
}
