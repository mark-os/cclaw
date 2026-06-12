/* Test session_set_state concurrency guard */
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>

static void test_valid_transitions(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "t", NULL, -1, 0);
    assert(sid > 0);

    /* idle → llm_running */
    assert(session_set_state(db, sid, "llm_running") == 0);
    /* llm_running → idle */
    assert(session_set_state(db, sid, "idle") == 0);

    /* idle → llm_running → error → idle */
    assert(session_set_state(db, sid, "llm_running") == 0);
    assert(session_set_state(db, sid, "idle") == 0);

    /* idle → tool_running → idle */
    assert(session_set_state(db, sid, "tool_running") == 0);
    assert(session_set_state(db, sid, "idle") == 0);

    db_close(db);
    printf("  PASS test_valid_transitions\n");
}

static void test_invalid_transitions(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "t", NULL, -1, 0);
    assert(sid > 0);

    /* idle → idle (no-op, rejected) */
    assert(session_set_state(db, sid, "idle") == -1);
    /* idle → waiting (invalid) */
    assert(session_set_state(db, sid, "waiting") == -1);
    /* idle → error (invalid) */
    assert(session_set_state(db, sid, "error") == -1);

    /* busy → busy is valid: a turn moves llm_running → tool_running →
     * llm_running, and ends llm_running → compacting */
    assert(session_set_state(db, sid, "llm_running") == 0);
    assert(session_set_state(db, sid, "tool_running") == 0);
    assert(session_set_state(db, sid, "llm_running") == 0);
    assert(session_set_state(db, sid, "compacting") == 0);
    assert(session_set_state(db, sid, "idle") == 0);

    /* unknown state names are rejected */
    assert(session_set_state(db, sid, "running") == -1);

    db_close(db);
    printf("  PASS test_invalid_transitions\n");
}

static void test_concurrent_acquire(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t s1 = session_create(db, "a", NULL, -1, 0);
    int64_t s2 = session_create(db, "b", NULL, -1, 0);
    assert(s1 > 0 && s2 > 0);

    /* Two different sessions can both go llm_running */
    assert(session_set_state(db, s1, "llm_running") == 0);
    assert(session_set_state(db, s2, "llm_running") == 0);

    /* Legacy state name is rejected */
    assert(session_set_state(db, s1, "running") == -1);

    db_close(db);
    printf("  PASS test_concurrent_acquire\n");
}

int main(void) {
    printf("test_session_state:\n");
    test_valid_transitions();
    test_invalid_transitions();
    test_concurrent_acquire();
    printf("All session state tests passed.\n");
    return 0;
}
