#include "advance.h"
#include "db.h"
#include "test_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

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

/* ── turn_id: one per user-visible exchange ───────────────────────── */

/* user msg → assistant tool call → tool result → assistant final:
 * one turn_id throughout, two distinct iteration_ids. */
static void test_turn_id_constant_across_iterations(void) {
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
    assert(turn > 0);
    assert(entry_col(db, e_a1, "turn_id") == turn);
    assert(entry_col(db, e_t1, "turn_id") == turn);
    assert(entry_col(db, e_a2, "turn_id") == turn);
    assert(it1 != it2);
    assert(entry_col(db, e_a1, "iteration_id") != entry_col(db, e_a2, "iteration_id"));

    db_close(db);
}

/* A second turn gets a new turn_id — including one that opens with several
 * inbox-drained user entries, which all land in that one turn. */
static void test_turn_id_changes_at_boundary(void) {
    sqlite3 *db = test_db_open(":memory:");
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message u = {.role = ROLE_USER, .content = "first"};
    int64_t e_u1 = entry_append_with_iteration(db, sid, &u, 0);
    Message a = {.role = ROLE_ASSISTANT, .content = "answer", .stop_reason = STOP_REASON_STOP};
    int64_t e_a1 = entry_append_with_iteration(db, sid, &a, 0);

    /* Turn 2 arrives as three inbox rows drained in one transaction. */
    inbox_insert(db, sid, "channel", NULL, "from chat");
    inbox_insert(db, sid, "job_result", NULL, "cron fired");
    inbox_insert(db, sid, "agent_result", NULL, "sub-agent done");
    assert(inbox_consume_into_entries(db, sid, 100) == 3);
    int64_t e_a2 = entry_append_with_iteration(db, sid, &a, 0);

    int64_t t1 = entry_col(db, e_u1, "turn_id");
    assert(entry_col(db, e_a1, "turn_id") == t1);

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
    assert(distinct == 1);
    assert(t2 != t1);
    assert(entry_col(db, e_a2, "turn_id") == t2);

    db_close(db);
}

/* A daemon that dies mid-turn leaves a non-idle session; the next process
 * resumes from the same leaf and must keep writing into the same turn. */
static void test_turn_id_survives_restart(void) {
    const char *path = "/tmp/test_cclaw_turn_restart.sqlite";
    test_db_clean(path);
    sqlite3 *db = test_db_open(path);
    test_seed_agent(db, "default");   /* agent_name is an enforced FK */
    int64_t sid = session_create(db, "test", "default", -1, 0);

    inbox_insert(db, sid, "channel", NULL, "please do the thing");
    AdvanceOutput out = advance_session(db, sid, 10);
    assert(out.action == ADVANCE_DISPATCH_LLM);

    ToolCall tc = {.id = "tc1", .name = "shell_exec", .arguments = "{}"};
    Message a1 = {.role = ROLE_ASSISTANT, .content = "running",
                  .stop_reason = STOP_REASON_TOOL_USE, .tool_calls = &tc, .tool_call_count = 1};
    int64_t e_a1 = entry_append_with_iteration(db, sid, &a1, db_next_iteration_id(db, sid));
    int64_t turn = entry_col(db, e_a1, "turn_id");
    int64_t leaf_before = leaf_of(db, sid);

    /* Crash: drop the handle mid-turn, leaving state='llm_running'. */
    db_close(db);
    db = test_db_open(path);
    assert(leaf_of(db, sid) == leaf_before);

    /* Resumed process writes the tool result and the final answer. */
    ToolResult tr = {.tool_call_id = "tc1", .content = "ok"};
    Message t1 = {.role = ROLE_TOOL, .tool_result = &tr, .tool_name = "shell_exec"};
    int64_t e_t1 = entry_append_with_iteration(db, sid, &t1, db_next_iteration_id(db, sid));
    Message a2 = {.role = ROLE_ASSISTANT, .content = "done", .stop_reason = STOP_REASON_STOP};
    int64_t e_a2 = entry_append_with_iteration(db, sid, &a2, db_next_iteration_id(db, sid));

    /* Resumed entries stay in the same turn. */
    assert(entry_col(db, e_t1, "turn_id") == turn);
    assert(entry_col(db, e_a2, "turn_id") == turn);

    db_close(db);
    test_db_clean(path);
}

int main(void) {
    TEST_INIT();
    printf("test_turn_tagging:\n");
    RUN(test_turn_id_constant_across_iterations);
    RUN(test_turn_id_changes_at_boundary);
    RUN(test_turn_id_survives_restart);
    return 0;
}
