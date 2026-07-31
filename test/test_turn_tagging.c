#include "advance.h"
#include "db.h"
#include "test_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

/* Helper: read one integer column of an entry (NULL → -1) */
static int64_t entry_col(sqlite3 *db, int64_t entry_id, const char *col) {
    char sql[96];
    snprintf(sql, sizeof(sql), "SELECT %s FROM entries WHERE id=?;", col);
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, entry_id);
    int64_t v = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL)
        v = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
}

static int64_t leaf_of(sqlite3 *db, int64_t sid) {
    return db_scalar_i64(db, "SELECT leaf_id FROM sessions WHERE id=?;", sid, -1);
}

/* ── iteration_id: minted per LLM request ─────────────────────────── */

static void test_next_iteration_id_empty_session(void) {
    TEST(next_iteration_id_empty_session);
    sqlite3 *db = test_db_open(":memory:");
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    int64_t tid = db_next_iteration_id(db, sid);
    if (tid != 1) { FAIL("expected 1 for empty session"); db_close(db); return; }
    db_close(db);
    PASS();
}

static void test_next_iteration_id_increments(void) {
    TEST(next_iteration_id_increments);
    sqlite3 *db = test_db_open(":memory:");
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message msg = {.role = ROLE_USER, .content = "hello"};
    entry_append_with_iteration(db, sid, &msg, 1);

    int64_t tid = db_next_iteration_id(db, sid);
    if (tid != 2) { FAIL("expected 2 after iteration_id=1"); db_close(db); return; }

    entry_append_with_iteration(db, sid, &msg, 2);
    tid = db_next_iteration_id(db, sid);
    if (tid != 3) { FAIL("expected 3 after iteration_id=2"); db_close(db); return; }

    db_close(db);
    PASS();
}

static void test_append_tags_iteration(void) {
    TEST(append_tags_iteration);
    sqlite3 *db = test_db_open(":memory:");
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message msg = {.role = ROLE_USER, .content = "hi"};
    int64_t eid = entry_append_with_iteration(db, sid, &msg, 42);
    if (eid < 0) { FAIL("append failed"); db_close(db); return; }

    if (entry_col(db, eid, "iteration_id") != 42) { FAIL("iteration_id not set"); db_close(db); return; }

    db_close(db);
    PASS();
}

static void test_multiple_entries_same_iteration(void) {
    TEST(multiple_entries_same_iteration);
    sqlite3 *db = test_db_open(":memory:");
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    /* One LLM response: assistant + tool_result share iteration_id=1 */
    Message asst = {.role = ROLE_ASSISTANT, .content = "calling tool"};
    int64_t e1 = entry_append_with_iteration(db, sid, &asst, 1);

    ToolResult tr = {.tool_call_id = "tc1", .content = "result"};
    Message tool_msg = {.role = ROLE_TOOL, .tool_result = &tr};
    int64_t e2 = entry_append_with_iteration(db, sid, &tool_msg, 1);

    if (entry_col(db, e1, "iteration_id") != 1) { FAIL("e1 iteration_id wrong"); db_close(db); return; }
    if (entry_col(db, e2, "iteration_id") != 1) { FAIL("e2 iteration_id wrong"); db_close(db); return; }

    /* Next iteration should be 2 */
    int64_t next = db_next_iteration_id(db, sid);
    if (next != 2) { FAIL("next iteration_id should be 2"); db_close(db); return; }

    db_close(db);
    PASS();
}

/* ── turn_id: one per user-visible exchange ───────────────────────── */

/* user msg → assistant tool call → tool result → assistant final:
 * one turn_id throughout, two distinct iteration_ids. */
static void test_turn_id_constant_across_iterations(void) {
    TEST(turn_id_constant_across_iterations);
    sqlite3 *db = test_db_open(":memory:");
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message u = {.role = ROLE_USER, .content = "do the thing"};
    int64_t e_user = entry_append_with_iteration(db, sid, &u, 0);

    /* iteration 1: assistant asks for a tool, tool answers */
    ToolCall tc = {.id = "tc1", .name = "shell_exec", .arguments = "{}"};
    Message a1 = {.role = ROLE_ASSISTANT, .content = "running",
                  .stop_reason = STOP_REASON_TOOL_USE, .tool_calls = &tc, .tool_call_count = 1};
    int64_t it1 = db_next_iteration_id(db, sid);
    int64_t e_a1 = entry_append_with_iteration(db, sid, &a1, it1);
    ToolResult tr = {.tool_call_id = "tc1", .content = "ok"};
    Message t1 = {.role = ROLE_TOOL, .tool_result = &tr, .tool_name = "shell_exec"};
    int64_t e_t1 = entry_append_with_iteration(db, sid, &t1, it1);

    /* iteration 2: final answer */
    int64_t it2 = db_next_iteration_id(db, sid);
    Message a2 = {.role = ROLE_ASSISTANT, .content = "done", .stop_reason = STOP_REASON_STOP};
    int64_t e_a2 = entry_append_with_iteration(db, sid, &a2, it2);

    int64_t turn = entry_col(db, e_user, "turn_id");
    if (turn <= 0) { FAIL("user entry has no turn_id"); db_close(db); return; }
    if (entry_col(db, e_a1, "turn_id") != turn ||
        entry_col(db, e_t1, "turn_id") != turn ||
        entry_col(db, e_a2, "turn_id") != turn) {
        FAIL("turn_id not carried across the turn"); db_close(db); return;
    }
    if (it1 == it2) { FAIL("two iterations should have distinct iteration_ids"); db_close(db); return; }
    if (entry_col(db, e_a1, "iteration_id") == entry_col(db, e_a2, "iteration_id")) {
        FAIL("assistant entries share an iteration_id"); db_close(db); return;
    }

    db_close(db);
    PASS();
}

/* A second turn gets a new turn_id — including one that opens with several
 * inbox-drained user entries, which all land in that one turn. */
static void test_turn_id_changes_at_boundary(void) {
    TEST(turn_id_changes_at_boundary);
    sqlite3 *db = test_db_open(":memory:");
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message u = {.role = ROLE_USER, .content = "first"};
    int64_t e_u1 = entry_append_with_iteration(db, sid, &u, 0);
    Message a = {.role = ROLE_ASSISTANT, .content = "answer", .stop_reason = STOP_REASON_STOP};
    int64_t e_a1 = entry_append_with_iteration(db, sid, &a, 0);

    /* Turn 2 arrives as three inbox rows drained in one transaction. */
    inbox_insert(db, sid, "channel", "from chat");
    inbox_insert(db, sid, "job_result", "cron fired");
    inbox_insert(db, sid, "agent_result", "sub-agent done");
    if (inbox_consume_into_entries(db, sid, 100) != 3) { FAIL("inbox drain"); db_close(db); return; }
    int64_t e_a2 = entry_append_with_iteration(db, sid, &a, 0);

    int64_t t1 = entry_col(db, e_u1, "turn_id");
    if (entry_col(db, e_a1, "turn_id") != t1) { FAIL("turn 1 not contiguous"); db_close(db); return; }

    /* The three drained entries + the reply are one new turn. */
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db,
        "SELECT COUNT(DISTINCT turn_id), MIN(turn_id) FROM entries"
        " WHERE session_id=? AND id > ?;", -1, &s, NULL);
    sqlite3_bind_int64(s, 1, sid);
    sqlite3_bind_int64(s, 2, e_a1);
    int distinct = 0; int64_t t2 = 0;
    if (sqlite3_step(s) == SQLITE_ROW) {
        distinct = sqlite3_column_int(s, 0);
        t2 = sqlite3_column_int64(s, 1);
    }
    sqlite3_finalize(s);
    if (distinct != 1) { FAIL("turn 2 split across several turn_ids"); db_close(db); return; }
    if (t2 == t1) { FAIL("turn 2 reused turn 1's id"); db_close(db); return; }
    if (entry_col(db, e_a2, "turn_id") != t2) { FAIL("reply not in turn 2"); db_close(db); return; }

    db_close(db);
    PASS();
}

/* A daemon that dies mid-turn leaves a non-idle session; the next process
 * resumes from the same leaf and must keep writing into the same turn. */
static void test_turn_id_survives_restart(void) {
    TEST(turn_id_survives_restart);
    const char *path = "/tmp/test_cclaw_turn_restart.sqlite";
    test_db_clean(path);
    sqlite3 *db = test_db_open(path);
    test_seed_agent(db, "default");   /* agent_name is an enforced FK */
    int64_t sid = session_create(db, "test", "default", -1, 0);

    inbox_insert(db, sid, "channel", "please do the thing");
    AdvanceOutput out = advance_session(db, sid, 10);
    if (out.action != ADVANCE_DISPATCH_LLM) { FAIL("turn did not open"); db_close(db); return; }

    ToolCall tc = {.id = "tc1", .name = "shell_exec", .arguments = "{}"};
    Message a1 = {.role = ROLE_ASSISTANT, .content = "running",
                  .stop_reason = STOP_REASON_TOOL_USE, .tool_calls = &tc, .tool_call_count = 1};
    int64_t e_a1 = entry_append_with_iteration(db, sid, &a1, db_next_iteration_id(db, sid));
    int64_t turn = entry_col(db, e_a1, "turn_id");
    int64_t leaf_before = leaf_of(db, sid);

    /* Crash: drop the handle mid-turn, leaving state='llm_running'. */
    db_close(db);
    db = test_db_open(path);
    if (leaf_of(db, sid) != leaf_before) { FAIL("leaf moved across restart"); db_close(db); return; }

    /* Resumed process writes the tool result and the final answer. */
    ToolResult tr = {.tool_call_id = "tc1", .content = "ok"};
    Message t1 = {.role = ROLE_TOOL, .tool_result = &tr, .tool_name = "shell_exec"};
    int64_t e_t1 = entry_append_with_iteration(db, sid, &t1, db_next_iteration_id(db, sid));
    Message a2 = {.role = ROLE_ASSISTANT, .content = "done", .stop_reason = STOP_REASON_STOP};
    int64_t e_a2 = entry_append_with_iteration(db, sid, &a2, db_next_iteration_id(db, sid));

    if (entry_col(db, e_t1, "turn_id") != turn || entry_col(db, e_a2, "turn_id") != turn) {
        FAIL("resumed entries opened a new turn"); db_close(db); return;
    }

    db_close(db);
    test_db_clean(path);
    PASS();
}

int main(void) {
    TEST_INIT();
    printf("test_turn_tagging:\n");
    test_next_iteration_id_empty_session();
    test_next_iteration_id_increments();
    test_append_tags_iteration();
    test_multiple_entries_same_iteration();
    test_turn_id_constant_across_iterations();
    test_turn_id_changes_at_boundary();
    test_turn_id_survives_restart();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
