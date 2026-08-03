/* The session_max_concurrent drain gate: defer-not-refuse turn opens, the
 * resource-holder counting rule (the nested-delegation deadlock trap), and
 * the human-batch / mid-turn-resume exemptions. */
#include "advance.h"
#include "config_registry.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sqlite3 *open_seeded(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    test_seed_agent(db, "default");
    return db;
}

static int64_t scalar(sqlite3 *db, const char *sql, int64_t arg) {
    return db_scalar_i64(db, sql, arg, -1);
}

static void exec_ok(sqlite3 *db, const char *sql) {
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
}

static void set_cap(sqlite3 *db, int cap) {
    char v[16];
    snprintf(v, sizeof(v), "%d", cap);
    assert(config_set(db, "session_max_concurrent", v) == 0);
}

static int state_is(sqlite3 *db, int64_t sid, const char *want) {
    char *s = db_scalar_text(db, "SELECT state FROM sessions WHERE id=?1;", sid);
    int ok = s && strcmp(s, want) == 0;
    free(s);
    return ok;
}

/* A session holding an LLM resource: an llm_jobs row, exactly what
 * llm_worker_submit persists before the pool takes the item. */
static int64_t holder_llm(sqlite3 *db) {
    int64_t sid = session_create(db, "holder", "default", -1, 0);
    assert(sid > 0);
    char sql[128];
    snprintf(sql, sizeof(sql),
             "INSERT INTO llm_jobs(session_id, agent_name) VALUES(%lld, 'default');",
             (long long)sid);
    exec_ok(db, sql);
    return sid;
}

/* A session with a running tool call — a forked/threaded tool child for any
 * real tool name, or a blocking sub-agent wait for 'launch_agent'. */
static int64_t running_call(sqlite3 *db, int64_t sid, const char *tool) {
    char sql[192];
    static int seq = 0;
    snprintf(sql, sizeof(sql),
             "INSERT INTO tool_calls(session_id, entry_id, call_id, name, status)"
             " VALUES(%lld, 0, 'cg_%d', '%s', 'running');",
             (long long)sid, ++seq, tool);
    exec_ok(db, sql);
    return sid;
}

/* Over the cap an all-autonomous batch defers — rows stay queued, no entry is
 * written, the caller learns it via out.deferred — and the freed resource is
 * the whole retry condition: delete it and the same advance opens the turn. */
static void test_defer_and_resume(void) {
    sqlite3 *db = open_seeded();
    set_cap(db, 2);
    holder_llm(db);
    int64_t h2 = holder_llm(db);

    int64_t sid = session_create(db, "t", "default", -1, 0);
    inbox_insert(db, sid, "cron", NULL, "fire");
    int64_t entries_before = scalar(db,
        "SELECT COUNT(*) FROM entries WHERE session_id=?1;", sid);

    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_NOOP);
    assert(out.deferred == 1);
    assert(inbox_count(db, sid) == 1);
    assert(state_is(db, sid, "idle"));
    assert(scalar(db, "SELECT COUNT(*) FROM entries WHERE session_id=?1;", sid)
           == entries_before);   /* a deferral writes nothing */

    /* A holder finishes (its worker deleted the job row): slot free. */
    char sql[96];
    snprintf(sql, sizeof(sql), "DELETE FROM llm_jobs WHERE session_id=%lld;",
             (long long)h2);
    exec_ok(db, sql);
    out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);
    assert(out.deferred == 0);
    assert(inbox_count(db, sid) == 0);

    db_close(db);
    printf("  PASS test_defer_and_resume\n");
}

/* A human anywhere in the pending batch opens the turn even over the cap. */
static void test_human_batch_passes(void) {
    sqlite3 *db = open_seeded();
    set_cap(db, 1);
    holder_llm(db);

    int64_t sid = session_create(db, "t", "default", -1, 0);
    inbox_insert(db, sid, "cron", NULL, "fire");
    inbox_insert(db, sid, "discord", NULL, "you there?");
    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);
    assert(out.deferred == 0);
    assert(inbox_count(db, sid) == 0);

    db_close(db);
    printf("  PASS test_human_batch_passes\n");
}

/* The deadlock regression. A parent blocked on blocking launch_agent sits in
 * tool_running with that call 'running' — it holds no resource, and must not
 * count: if waiting parents held slots, nested delegation at the cap would
 * wedge (parents hold every slot → children never start → parents never
 * finish). A running call for any *real* tool is a live child and counts. */
static void test_blocked_parent_is_not_a_holder(void) {
    sqlite3 *db = open_seeded();
    set_cap(db, 1);

    int64_t parent = session_create(db, "p", "default", -1, 0);
    session_set_state(db, parent, "llm_running");
    session_set_state(db, parent, "tool_running");
    running_call(db, parent, "launch_agent");

    int64_t child = session_create(db, "c", "default", parent, 1);
    inbox_insert(db, child, "spawn", NULL, "the task");
    AdvanceOutput out = advance_session(db, child, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);   /* parent held nothing */

    /* Same shape, but the parent's running call is a real forked tool:
     * that is a live child process, and it holds the one slot. */
    int64_t parent2 = session_create(db, "p2", "default", -1, 0);
    running_call(db, parent2, "shell_exec");
    int64_t child2 = session_create(db, "c2", "default", parent2, 1);
    inbox_insert(db, child2, "spawn", NULL, "another task");
    out = advance_session(db, child2, 25);
    assert(out.action == ADVANCE_NOOP);
    assert(out.deferred == 1);

    db_close(db);
    printf("  PASS test_blocked_parent_is_not_a_holder\n");
}

/* A role-3 leaf is a half-spent turn resuming after a mid-turn bounce —
 * exempt from the gate: deferring it strands paid-for work. */
static void test_role3_resume_exempt(void) {
    sqlite3 *db = open_seeded();
    set_cap(db, 1);
    holder_llm(db);

    int64_t sid = session_create(db, "t", "default", -1, 0);
    inbox_insert(db, sid, "cli", NULL, "do it");
    assert(inbox_consume_into_entries(db, sid, 10) == 1);
    entry_append_typed(db, sid, 1, "assistant_message", 0, NULL, NULL, NULL, 0,
                       STOP_REASON_TOOL_USE, NULL, 0, 0, 0);
    int64_t eid = entry_append_typed(db, sid, 1, "tool_call", 0,
                                     "{\"command\":\"x\"}", "call_1", "shell_exec",
                                     0, STOP_REASON_NONE, NULL, 0, 0, 0);
    assert(eid > 0);
    char ins[192];
    snprintf(ins, sizeof(ins),
             "INSERT INTO tool_calls(session_id, entry_id, call_id, name, status)"
             " VALUES(%lld, %lld, 'call_1', 'shell_exec', 'done');",
             (long long)sid, (long long)eid);
    exec_ok(db, ins);
    Message res = { .role = ROLE_TOOL, .content = "output" };
    ToolResult tr = { .tool_call_id = "call_1", .content = "output" };
    res.tool_result = &tr;
    res.tool_name = "shell_exec";
    entry_append_with_iteration(db, sid, &res, 1);

    /* The bounce parked it idle with the tool result as leaf. */
    session_set_state(db, sid, "llm_running");
    session_set_state(db, sid, "idle");
    assert(scalar(db, "SELECT role FROM entries"
                      " WHERE id=(SELECT leaf_id FROM sessions WHERE id=?1);",
                  sid) == ROLE_TOOL);

    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);
    assert(out.iteration == 1);   /* same turn, not a fresh one */

    db_close(db);
    printf("  PASS test_role3_resume_exempt\n");
}

/* A role-1 leaf is a turn that never dispatched (refused-dispatch repark).
 * Stamped autonomous it is gated like a fresh open; unstamped it is human
 * (only the drain stamps) and passes. */
static void test_role1_leaf_gated_by_stamp(void) {
    sqlite3 *db = open_seeded();
    set_cap(db, 1);

    /* Drain a cron row for real so the entry carries the drain's stamp, then
     * simulate main.c's dispatch refusal reverting the session to idle. */
    int64_t sid = session_create(db, "t", "default", -1, 0);
    inbox_insert(db, sid, "cron", NULL, "fire");
    assert(advance_session(db, sid, 25).action == ADVANCE_DISPATCH_LLM);
    session_set_state(db, sid, "idle");
    assert(scalar(db, "SELECT role FROM entries"
                      " WHERE id=(SELECT leaf_id FROM sessions WHERE id=?1);",
                  sid) == ROLE_USER);

    holder_llm(db);
    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_NOOP);
    assert(out.deferred == 1);
    assert(state_is(db, sid, "idle"));

    /* Unstamped role-1 leaf: typed straight in — human, passes over cap. */
    int64_t sid2 = session_create(db, "t2", "default", -1, 0);
    Message m = { .role = ROLE_USER, .content = "typed straight in" };
    assert(entry_append_with_iteration(db, sid2, &m, 0) > 0);
    out = advance_session(db, sid2, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);
    assert(out.deferred == 0);

    db_close(db);
    printf("  PASS test_role1_leaf_gated_by_stamp\n");
}

/* The session's own stale llm_jobs row must not count against it — it can
 * never block itself, else one leaked row wedges it out of every slot. */
static void test_own_stale_job_excluded(void) {
    sqlite3 *db = open_seeded();
    set_cap(db, 1);

    int64_t sid = session_create(db, "t", "default", -1, 0);
    char sql[128];
    snprintf(sql, sizeof(sql),
             "INSERT INTO llm_jobs(session_id, agent_name) VALUES(%lld, 'default');",
             (long long)sid);
    exec_ok(db, sql);
    inbox_insert(db, sid, "cron", NULL, "fire");

    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);

    db_close(db);
    printf("  PASS test_own_stale_job_excluded\n");
}

/* The compacting branch is the idle branch's twin: same gate, same deferral —
 * and the idle flip stands either way (compaction is done regardless). */
static void test_compacting_twin_deferred(void) {
    sqlite3 *db = open_seeded();
    set_cap(db, 1);
    holder_llm(db);

    int64_t sid = session_create(db, "t", "default", -1, 0);
    inbox_insert(db, sid, "cron", NULL, "fire");
    assert(session_set_state(db, sid, "compacting") == 0);
    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_NOOP);
    assert(out.deferred == 1);
    assert(state_is(db, sid, "idle"));
    assert(inbox_count(db, sid) == 1);

    /* Human in the batch: the twin opens it over the cap too. */
    inbox_insert(db, sid, "cli", NULL, "hi");
    assert(session_set_state(db, sid, "compacting") == 0);
    out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);
    assert(inbox_count(db, sid) == 0);

    db_close(db);
    printf("  PASS test_compacting_twin_deferred\n");
}

/* cap 0 = gate off, however many holders exist. */
static void test_cap_zero_off(void) {
    sqlite3 *db = open_seeded();
    set_cap(db, 0);
    holder_llm(db);
    holder_llm(db);

    int64_t sid = session_create(db, "t", "default", -1, 0);
    inbox_insert(db, sid, "cron", NULL, "fire");
    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);
    assert(out.deferred == 0);

    db_close(db);
    printf("  PASS test_cap_zero_off\n");
}

int main(void) {
    TEST_INIT();
    printf("test_concurrency_gate:\n");
    test_defer_and_resume();
    test_human_batch_passes();
    test_blocked_parent_is_not_a_holder();
    test_role3_resume_exempt();
    test_role1_leaf_gated_by_stamp();
    test_own_stale_job_excluded();
    test_compacting_twin_deferred();
    test_cap_zero_off();
    printf("All concurrency gate tests passed.\n");
    return 0;
}
