/* Test advance_session decision logic */
#include "advance.h"
#include "db.h"
#include "test_util.h"
#include "tool_agent.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* agent_name is an enforced FK (v31): seed the parent row before sessions. */
static sqlite3 *open_seeded(const char *path) {
    sqlite3 *db = test_db_open(path);
    if (db) test_seed_agent(db, "default");
    return db;
}

static void test_idle_no_inbox(void) {
    sqlite3 *db = open_seeded(":memory:");
    int64_t sid = session_create(db, "test", "default", -1, 0);
    assert(sid > 0);

    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_NOOP);
    assert(out.session_id == sid);

    db_close(db);
    printf("  PASS test_idle_no_inbox\n");
}

static void test_idle_with_inbox(void) {
    sqlite3 *db = open_seeded(":memory:");
    int64_t sid = session_create(db, "test", "default", -1, 0);
    inbox_insert(db, sid, "cli", NULL, "hello");

    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);
    assert(out.iteration == 0);
    assert(strcmp(out.agent_name, "default") == 0);
    /* Inbox should be consumed */
    assert(inbox_count(db, sid) == 0);
    /* Iteration should be 0 */
    assert((int)db_scalar_i64(db, "SELECT turn_iteration FROM sessions WHERE id=?", sid, -1) == 0);

    db_close(db);
    printf("  PASS test_idle_with_inbox\n");
}

static void test_llm_complete_stop(void) {
    sqlite3 *db = open_seeded(":memory:");
    int64_t sid = session_create(db, "test", "default", -1, 0);

    /* Simulate: session in llm_running, LLM wrote a stop entry */
    session_set_state(db, sid, "llm_running");
    Message msg = { .role = ROLE_ASSISTANT, .content = "done",
                    .stop_reason = STOP_REASON_STOP };
    int64_t iteration_id = db_next_iteration_id(db, sid);
    entry_append_with_iteration(db, sid, &msg, iteration_id);

    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_DONE);

    db_close(db);
    printf("  PASS test_llm_complete_stop\n");
}

static void test_tool_running_all_done(void) {
    sqlite3 *db = open_seeded(":memory:");
    int64_t sid = session_create(db, "test", "default", -1, 0);

    /* Session in tool_running, no pending tool_calls, iteration 2 */
    session_set_state(db, sid, "llm_running");
    session_set_state(db, sid, "tool_running");
    session_set_iteration(db, sid, 2);

    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);
    assert(out.iteration == 3); /* bumped from 2 */
    assert((int)db_scalar_i64(db, "SELECT turn_iteration FROM sessions WHERE id=?", sid, -1) == 3);

    db_close(db);
    printf("  PASS test_tool_running_all_done\n");
}

static void test_max_iterations(void) {
    sqlite3 *db = open_seeded(":memory:");
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
    sqlite3 *db = open_seeded(":memory:");
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

static void test_idle_unanswered_user_leaf(void) {
    sqlite3 *db = open_seeded(":memory:");
    int64_t sid = session_create(db, "test", "default", -1, 0);

    /* Simulate a refused dispatch: inbox already consumed into entries (leaf
     * is a user entry), session parked idle, inbox empty — the turn must be
     * re-dispatched, not stranded. */
    Message msg = { .role = ROLE_USER, .content = "hello" };
    entry_append_with_iteration(db, sid, &msg, db_next_iteration_id(db, sid));
    session_set_iteration(db, sid, 5);

    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);
    assert(out.iteration == 0);
    assert((int)db_scalar_i64(db, "SELECT turn_iteration FROM sessions WHERE id=?", sid, -1) == 0);

    db_close(db);
    printf("  PASS test_idle_unanswered_user_leaf\n");
}

static void test_idle_answered_leaf_noop(void) {
    sqlite3 *db = open_seeded(":memory:");
    int64_t sid = session_create(db, "test", "default", -1, 0);

    /* Completed turn: assistant entry is the leaf — idle stays NOOP. */
    Message umsg = { .role = ROLE_USER, .content = "hello" };
    int64_t iteration_id = db_next_iteration_id(db, sid);
    entry_append_with_iteration(db, sid, &umsg, iteration_id);
    Message amsg = { .role = ROLE_ASSISTANT, .content = "hi",
                     .stop_reason = STOP_REASON_STOP };
    entry_append_with_iteration(db, sid, &amsg, iteration_id);

    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_NOOP);

    db_close(db);
    printf("  PASS test_idle_answered_leaf_noop\n");
}

/* ── D11: stale tool_calls are reconciled when a turn opens ───────────── */

/* Write one tool call the way db_response.c does: a tool_call entry carrying
 * the arguments, plus its tool_calls workflow row. Returns the entry id. */
static int64_t append_tool_call(sqlite3 *db, int64_t sid, int64_t iteration,
                                const char *name, const char *call_id,
                                const char *args, int part) {
    int64_t eid = entry_append_typed(db, sid, iteration, "tool_call", part, args,
                                     call_id, name, 0, STOP_REASON_NONE,
                                     NULL, 0, 0, 0);
    assert(eid > 0);
    char ins[256];
    snprintf(ins, sizeof(ins),
             "INSERT INTO tool_calls(session_id, entry_id, call_id, name)"
             " VALUES(%lld, %lld, '%s', '%s');",
             (long long)sid, (long long)eid, call_id, name);
    assert(sqlite3_exec(db, ins, NULL, NULL, NULL) == SQLITE_OK);
    return eid;
}

static int64_t count(sqlite3 *db, const char *sql, int64_t arg) {
    return db_scalar_i64(db, sql, arg, -1);
}

/* A call orphaned by a crash (left 'pending', session forced idle without
 * recovery) must be answered and closed when the next turn opens — not
 * re-dispatched ahead of that turn's own calls with its stale arguments. */
static void test_stale_call_reconciled_at_turn_start(void) {
    sqlite3 *db = open_seeded(":memory:");
    int64_t sid = session_create(db, "test", "default", -1, 0);

    /* Turn 1: user message, then the model asks for a tool — and the process
     * dies before the tool answers, leaving the session idle with a pending
     * call and no recovery pass. */
    Message u = { .role = ROLE_USER, .content = "turn one" };
    entry_append_with_iteration(db, sid, &u, 1);
    entry_append_typed(db, sid, 1, "assistant_message", 0, NULL, NULL, NULL, 0,
                       STOP_REASON_TOOL_USE, NULL, 0, 0, 0);
    int64_t stale_eid = append_tool_call(db, sid, 1, "shell_exec", "call_stale",
                                         "{\"command\":\"old\"}", 1);
    int64_t turn1 = count(db, "SELECT turn_id FROM entries WHERE id=?", stale_eid);
    assert(turn1 > 0);

    /* Turn 2 opens. */
    inbox_insert(db, sid, "cli", NULL, "turn two");
    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);

    /* The orphan is closed under its own tag, with a result the model can see. */
    assert(count(db, "SELECT COUNT(*) FROM tool_calls WHERE call_id='call_stale'"
                     " AND status='done' AND resolved_by='stale:turn-start';", 0) == 1);
    assert(count(db, "SELECT COUNT(*) FROM entries WHERE role=3"
                     " AND tool_call_id='call_stale' AND is_error=1"
                     " AND content LIKE 'error: superseded%';", 0) == 1);

    /* It landed in the old turn, before the new user entry — a tool result
     * appended after the turn opened would break provider adjacency. */
    assert(count(db, "SELECT turn_id FROM entries WHERE tool_call_id='call_stale'"
                     " AND role=3;", 0) == turn1);
    assert(count(db, "SELECT iteration_id FROM entries WHERE tool_call_id='call_stale'"
                     " AND role=3;", 0) == 1);
    assert(count(db, "SELECT turn_id FROM entries WHERE content='turn two';", 0) != turn1);

    /* Turn 2's own tool call is the only thing dispatched. */
    append_tool_call(db, sid, 2, "shell_exec", "call_live", "{\"command\":\"new\"}", 1);
    out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_DISPATCH_TOOLS);
    assert(out.tc_count == 1);
    assert(strcmp(out.calls[0].call_id, "call_live") == 0);
    db_tool_call_free_pending(out.calls, out.tc_count);

    db_close(db);
    printf("  PASS test_stale_call_reconciled_at_turn_start\n");
}

/* The one deliberate skip: an unanswered user leaf means the turn is already
 * open and merely undispatched (refused dispatch), so nothing is superseded —
 * and a tool result appended there would steal the leaf that turn is waiting
 * on. The turn must still dispatch. (The stale call is picked up by the next
 * turn open, whose leaf is no longer a user entry.) */
static void test_open_undispatched_turn_not_stranded(void) {
    sqlite3 *db = open_seeded(":memory:");
    int64_t sid = session_create(db, "test", "default", -1, 0);

    Message u1 = { .role = ROLE_USER, .content = "turn one" };
    entry_append_with_iteration(db, sid, &u1, 1);
    entry_append_typed(db, sid, 1, "assistant_message", 0, NULL, NULL, NULL, 0,
                       STOP_REASON_TOOL_USE, NULL, 0, 0, 0);
    append_tool_call(db, sid, 1, "shell_exec", "call_stale", "{}", 1);
    /* Turn 2's user entry made it into entries, then dispatch was refused. */
    Message u2 = { .role = ROLE_USER, .content = "turn two" };
    entry_append_with_iteration(db, sid, &u2, 2);

    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);
    assert(count(db, "SELECT COUNT(*) FROM entries WHERE role=3;", 0) == 0);
    assert(count(db, "SELECT leaf_id FROM sessions WHERE id=?;", sid) ==
           count(db, "SELECT id FROM entries WHERE content='turn two';", 0));

    db_close(db);
    printf("  PASS test_open_undispatched_turn_not_stranded\n");
}

/* Health check for the same code: two blocking sub-agents keep the parent in
 * tool_running with both calls 'running'. The parent is never idle while they
 * are in flight, so reconciliation must never see them — and once they answer,
 * the next turn opening must leave their results alone. */
static void test_parallel_subagents_never_reconciled(void) {
    sqlite3 *db = open_seeded(":memory:");
    int64_t parent = session_create(db, "parent", "default", -1, 0);

    /* One assistant response with two parallel launch_agent calls. */
    Message u = { .role = ROLE_USER, .content = "delegate twice" };
    entry_append_with_iteration(db, parent, &u, 1);
    entry_append_typed(db, parent, 1, "assistant_message", 0, NULL, NULL, NULL, 0,
                       STOP_REASON_TOOL_USE, NULL, 0, 0, 0);
    append_tool_call(db, parent, 1, "launch_agent", "call_a", "{\"task\":\"a\"}", 1);
    append_tool_call(db, parent, 1, "launch_agent", "call_b", "{\"task\":\"b\"}", 2);

    /* Dispatch the batch the way the daemon does: claim, then run. Blocking
     * launch_agent returns NULL — the child answers on completion. */
    session_set_state(db, parent, "llm_running");
    session_set_state(db, parent, "tool_running");
    const char *ids[2] = { "call_a", "call_b" };
    for (int i = 0; i < 2; i++) {
        assert(db_tool_call_claim(db, parent, ids[i]) == 1);
        AgentLaunchCtx ctx = { .db = db, .session_id = parent,
                               .current_tool_call_id = ids[i] };
        assert(tool_launch_agent_handler("{\"task\":\"work\"}", &ctx) == NULL);
    }
    assert(count(db, "SELECT COUNT(*) FROM tool_calls WHERE session_id=?"
                     " AND status='running';", parent) == 2);

    /* Both async: the parent parks in tool_running, never idle. */
    AdvanceOutput out = advance_session(db, parent, 25);
    assert(out.action == ADVANCE_WAITING);

    /* Children finish one at a time; each notify_parent answers its own call. */
    int64_t kid[2] = {0, 0};
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db, "SELECT id FROM sessions WHERE parent_session_id=?"
                                  " ORDER BY id;", -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, parent);
    for (int i = 0; i < 2 && sqlite3_step(s) == SQLITE_ROW; i++)
        kid[i] = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    assert(kid[0] > 0 && kid[1] > 0);

    for (int i = 0; i < 2; i++) {
        /* Consume the spawn task first, as a real child's turn does — an
         * unconsumed inbox row correctly holds a quiescent one-shot. */
        assert(inbox_consume_into_entries(db, kid[i], 10) == 1);
        session_set_state(db, kid[i], "llm_running");
        Message done = { .role = ROLE_ASSISTANT, .content = "child result",
                         .stop_reason = STOP_REASON_STOP };
        entry_append_with_iteration(db, kid[i], &done, 1);
        assert(advance_session(db, kid[i], 25).action == ADVANCE_DONE);
        out = advance_session(db, parent, 25);
        /* First completion: the sibling is still running — keep waiting. */
        assert(out.action == (i == 0 ? ADVANCE_WAITING : ADVANCE_DISPATCH_LLM));
    }

    /* Both answered by their children, neither touched by reconciliation. */
    assert(count(db, "SELECT COUNT(*) FROM tool_calls WHERE session_id=?"
                     " AND status='done' AND resolved_by IS NULL;", parent) == 2);

    /* Finish the turn and open a new one — the healthy calls stay untouched. */
    Message final = { .role = ROLE_ASSISTANT, .content = "both back",
                      .stop_reason = STOP_REASON_STOP };
    entry_append_with_iteration(db, parent, &final, 2);
    assert(advance_session(db, parent, 25).action == ADVANCE_DONE);
    inbox_insert(db, parent, "cli", NULL, "next turn");
    assert(advance_session(db, parent, 25).action == ADVANCE_DISPATCH_LLM);

    assert(count(db, "SELECT COUNT(*) FROM tool_calls"
                     " WHERE resolved_by='stale:turn-start';", 0) == 0);
    assert(count(db, "SELECT COUNT(*) FROM entries"
                     " WHERE content LIKE 'error: superseded%';", 0) == 0);

    db_close(db);
    printf("  PASS test_parallel_subagents_never_reconciled\n");
}

/* A background child's full result must reach the parent inbox — the notice
 * used to squeeze it through a 256-byte buffer (200-char cap), which cost a
 * real user half their answer (smoke test, 2026-08-01). */
static void test_background_result_not_truncated(void) {
    sqlite3 *db = open_seeded(":memory:");
    int64_t parent = session_create(db, "parent", "default", -1, 0);
    int64_t child = session_create(db, "agent", "default", parent, 1);
    assert(child > 0);
    /* No parent_tool_call_id: this is the background path. */

    char result[512];
    memset(result, 'x', sizeof(result));
    memcpy(result + sizeof(result) - 9, "TAIL_END", 9);
    session_set_state(db, child, "llm_running");
    Message done = { .role = ROLE_ASSISTANT, .content = result,
                     .stop_reason = STOP_REASON_STOP };
    entry_append_with_iteration(db, child, &done, 1);
    assert(advance_session(db, child, 25).action == ADVANCE_DONE);

    /* The id is structural (source_ref), not prose. */
    char want[600], child_str[24];
    snprintf(want, sizeof(want), "Sub-agent completed: %s", result);
    snprintf(child_str, sizeof(child_str), "%lld", (long long)child);
    assert(count(db, "SELECT COUNT(*) FROM inbox WHERE session_id=?"
                     " AND source='agent_result';", parent) == 1);
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db, "SELECT payload, source_ref FROM inbox"
                                  " WHERE session_id=? AND source='agent_result';",
                              -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, parent);
    assert(sqlite3_step(s) == SQLITE_ROW);
    const char *payload = (const char *)sqlite3_column_text(s, 0);
    const char *ref = (const char *)sqlite3_column_text(s, 1);
    assert(payload && strcmp(payload, want) == 0);
    assert(ref && strcmp(ref, child_str) == 0);
    sqlite3_finalize(s);

    /* Draining re-attaches the id as a tag: the parent still needs it for
     * check_session, and the payload stays whole behind it. */
    char tagged[768];
    snprintf(tagged, sizeof(tagged), "[sub-agent session %s] %s", child_str, want);
    assert(inbox_consume_into_entries(db, parent, 10) == 1);
    assert(sqlite3_prepare_v2(db, "SELECT content, content_bytes FROM entries"
                                  " WHERE session_id=? AND role=1 ORDER BY id DESC LIMIT 1;",
                              -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, parent);
    assert(sqlite3_step(s) == SQLITE_ROW);
    const char *content = (const char *)sqlite3_column_text(s, 0);
    assert(content && strcmp(content, tagged) == 0);
    assert(sqlite3_column_int(s, 1) == (int)strlen(tagged));
    sqlite3_finalize(s);

    db_close(db);
    printf("  PASS test_background_result_not_truncated\n");
}

/* ── The convergence sweep: cursor on delivery, sweep the lagging edges ── */

/* Finish a child's turn the way the daemon does: llm_running + an assistant
 * leaf, then advance. Returns the advance action. */
static AdvanceResult finish_child(sqlite3 *db, int64_t child, char *text,
                                  StopReason sr) {
    session_set_state(db, child, "llm_running");
    Message done = { .role = ROLE_ASSISTANT, .content = text, .stop_reason = sr };
    entry_append_with_iteration(db, child, &done, 1);
    return advance_session(db, child, 25).action;
}

/* The standing parent edge's cursor; 0 = nothing delivered yet. */
static int64_t cursor_of(sqlite3 *db, int64_t sid) {
    return db_scalar_i64(db,
        "SELECT cursor FROM delivery_edges WHERE session_id=?"
        " AND target_kind='parent' AND one_shot=0;", sid, -1);
}

/* Wire up a blocking launch the way the real handler does: the parent's
 * tool_calls row plus the child's one-shot reply edge. */
static void make_blocking(sqlite3 *db, int64_t parent, int64_t child,
                          const char *call_id) {
    char ins[256];
    snprintf(ins, sizeof(ins),
        "INSERT INTO tool_calls(session_id, entry_id, call_id, name, status)"
        " VALUES(%lld, 1, '%s', 'launch_agent', 'running');",
        (long long)parent, call_id);
    assert(sqlite3_exec(db, ins, NULL, NULL, NULL) == SQLITE_OK);
    session_set_parent_tool_call_id(db, child, call_id);
    assert(delivery_edge_create(db, child, "tool_call", call_id,
                                "quiescent", 1) == 0);
}

/* Every delivery stamps the edge cursor inside its own transaction — the
 * cursor at the leaf is what tells the sweep this push landed. */
static void test_delivery_stamps_cursor(void) {
    sqlite3 *db = open_seeded(":memory:");
    int64_t parent = session_create(db, "parent", "default", -1, 0);

    /* Blocking: the parent's launch_agent call gets a tool result and the
     * one-shot edge dies with the firing; the standing cursor advances in
     * the same transaction so nothing ships twice. */
    int64_t blocking = session_create(db, "blocking", "default", parent, 1);
    make_blocking(db, parent, blocking, "call_b");
    assert(finish_child(db, blocking, "blocking answer", STOP_REASON_STOP) == ADVANCE_DONE);
    assert(cursor_of(db, blocking) > 0);
    assert(count(db, "SELECT COUNT(*) FROM delivery_edges WHERE session_id=?"
                     " AND one_shot=1;", blocking) == 0);
    assert(count(db, "SELECT COUNT(*) FROM entries WHERE session_id=? AND role=3;",
                 parent) == 1);
    assert(count(db, "SELECT COUNT(*) FROM inbox WHERE session_id=?"
                     " AND source='agent_result';", parent) == 0);

    /* Background: the answer goes to the parent inbox instead. */
    int64_t background = session_create(db, "background", "default", parent, 1);
    assert(finish_child(db, background, "background answer", STOP_REASON_STOP) == ADVANCE_DONE);
    assert(cursor_of(db, background) > 0);
    assert(count(db, "SELECT COUNT(*) FROM inbox WHERE session_id=?"
                     " AND source='agent_result';", parent) == 1);

    db_close(db);
    printf("  PASS test_delivery_stamps_cursor\n");
}

/* A push that never landed leaves the cursor behind the leaf — the durable
 * fact the sweep re-derives from. It re-delivers once, then stays quiet. */
static void test_sweep_redelivers_lost_push(void) {
    sqlite3 *db = open_seeded(":memory:");
    int64_t parent = session_create(db, "parent", "default", -1, 0);
    int64_t child = session_create(db, "child", "default", parent, 1);
    assert(finish_child(db, child, "the answer", STOP_REASON_STOP) == ADVANCE_DONE);

    /* Simulate the lost transaction: cursor back to 0, no inbox row. */
    char sql[256];
    snprintf(sql, sizeof(sql),
             "UPDATE delivery_edges SET cursor=0 WHERE session_id=%lld;"
             "DELETE FROM inbox WHERE session_id=%lld AND source='agent_result';",
             (long long)child, (long long)parent);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);

    assert(advance_sweep_undelivered(db) == 1);
    assert(count(db, "SELECT COUNT(*) FROM inbox WHERE session_id=?"
                     " AND source='agent_result';", parent) == 1);
    assert(cursor_of(db, child) > 0);

    /* Idempotent by the cursor: a second tick re-delivers nobody. */
    assert(advance_sweep_undelivered(db) == 0);
    assert(count(db, "SELECT COUNT(*) FROM inbox WHERE session_id=?"
                     " AND source='agent_result';", parent) == 1);

    /* A child that never finished a turn is not a lost push: no entries at all,
     * or a role-1 leaf waiting on its first request. Both stay unswept. */
    int64_t fresh = session_create(db, "fresh", "default", parent, 1);
    assert(advance_sweep_undelivered(db) == 0);
    Message task = { .role = ROLE_USER, .content = "work on this" };
    entry_append_with_iteration(db, fresh, &task, 1);
    assert(advance_sweep_undelivered(db) == 0);
    assert(cursor_of(db, fresh) == 0);

    db_close(db);
    printf("  PASS test_sweep_redelivers_lost_push\n");
}

/* An errored child must tell its parent so. entries.stop_reason is an INTEGER;
 * comparing it as text against "error" (until 2026-08-01) never matched, so
 * every failed sub-agent reported success. */
static void test_error_stop_reaches_parent(void) {
    sqlite3 *db = open_seeded(":memory:");
    int64_t parent = session_create(db, "parent", "default", -1, 0);

    /* Blocking: the tool result carries is_error=1. */
    int64_t blocking = session_create(db, "blocking", "default", parent, 1);
    make_blocking(db, parent, blocking, "call_b");
    assert(finish_child(db, blocking, "provider exploded", STOP_REASON_ERROR) == ADVANCE_ERROR);
    assert(count(db, "SELECT COUNT(*) FROM entries WHERE session_id=?"
                     " AND role=3 AND is_error=1;", parent) == 1);

    /* Background: the inbox notice says failed, not completed. */
    int64_t background = session_create(db, "background", "default", parent, 1);
    assert(finish_child(db, background, "provider exploded", STOP_REASON_ERROR) == ADVANCE_ERROR);
    assert(count(db, "SELECT COUNT(*) FROM inbox WHERE session_id=?"
                     " AND source='agent_result'"
                     " AND payload LIKE 'Sub-agent failed:%';", parent) == 1);

    db_close(db);
    printf("  PASS test_error_stop_reaches_parent\n");
}

/* A mid-turn dispatch bounce (worker pool full) reverts the session to idle
 * with an unanswered tool result as the leaf. That turn must resume — and
 * resume as the *same* turn: iteration re-derived from the entries (the bounce
 * zeroed sessions.turn_iteration), frozen turn_context untouched, so the
 * dispatch runs with recall=0 and reuses it. */
static void test_idle_unanswered_tool_leaf_resumes(void) {
    sqlite3 *db = open_seeded(":memory:");
    int64_t sid = session_create(db, "test", "default", -1, 0);

    /* One turn, two LLM requests. The user entry arrives by inbox drain, so it
     * carries no iteration_id — only real requests count. */
    inbox_insert(db, sid, "cli", NULL, "do it");
    assert(inbox_consume_into_entries(db, sid, 10) == 1);
    for (int64_t it = 1; it <= 2; it++) {
        entry_append_typed(db, sid, it, "assistant_message", 0, NULL, NULL, NULL, 0,
                           STOP_REASON_TOOL_USE, NULL, 0, 0, 0);
        char call[16];
        snprintf(call, sizeof(call), "call_%lld", (long long)it);
        append_tool_call(db, sid, it, "shell_exec", call, "{\"command\":\"x\"}", 1);
        Message res = { .role = ROLE_TOOL, .content = "output" };
        ToolResult tr = { .tool_call_id = call, .content = "output" };
        res.tool_result = &tr;
        res.tool_name = "shell_exec";
        entry_append_with_iteration(db, sid, &res, it);
    }
    assert(sqlite3_exec(db, "UPDATE tool_calls SET status='done';",
                        NULL, NULL, NULL) == SQLITE_OK);
    assert(count(db, "SELECT role FROM entries WHERE id="
                     "(SELECT leaf_id FROM sessions WHERE id=?);", sid) == ROLE_TOOL);

    /* The bounce: request #3 was refused, so main.c reverted the session to
     * idle — which zeroed turn_iteration. turn_context stays frozen. */
    session_set_turn_context(db, sid, "<FROZEN/>");
    session_set_iteration(db, sid, 2);
    session_set_state(db, sid, "llm_running");
    session_set_state(db, sid, "idle");
    assert(count(db, "SELECT turn_iteration FROM sessions WHERE id=?;", sid) == 0);

    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);
    /* Two requests already spent → this is iteration 2, not a fresh turn.
     * dispatch_llm_req turns iteration>0 into recall=0 (main.c), which is what
     * makes the request reuse the frozen context below. */
    assert(out.iteration == 2);
    assert(count(db, "SELECT turn_iteration FROM sessions WHERE id=?;", sid) == 2);
    char *ctx = session_get_turn_context(db, sid);
    assert(ctx && strcmp(ctx, "<FROZEN/>") == 0);
    free(ctx);
    /* Same turn, not a new one: no user entry was invented. */
    assert(count(db, "SELECT COUNT(DISTINCT turn_id) FROM entries WHERE session_id=?;",
                 sid) == 1);

    db_close(db);
    printf("  PASS test_idle_unanswered_tool_leaf_resumes\n");
}

/* llm_running with no job row and a non-assistant leaf = the submit was lost
 * (pool-full revert dropped its idle write on a BUSY give-up). The old
 * behavior misread the *previous* turn's assistant entry as this turn's
 * completion — ADVANCE_DONE, stale delivery, premature parent notify. Must
 * re-park idle instead; the sweep's unanswered-leaf arm then resumes. */
static void test_lost_submit_reparks_idle(void) {
    sqlite3 *db = open_seeded(":memory:");
    int64_t sid = session_create(db, "test", "default", -1, 0);

    /* Turn 1 completed normally: assistant answer on the branch. */
    Message a1 = { .role = ROLE_ASSISTANT, .content = "turn one answer",
                   .stop_reason = STOP_REASON_STOP };
    entry_append_with_iteration(db, sid, &a1, db_next_iteration_id(db, sid));
    /* Turn 2 opened (inbox consumed into a user entry) but never submitted. */
    Message u2 = { .role = ROLE_USER, .content = "turn two question" };
    entry_append_with_iteration(db, sid, &u2, db_next_iteration_id(db, sid));
    session_set_state(db, sid, "llm_running");

    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_NOOP);
    assert(db_scalar_i64(db,
        "SELECT count(*) FROM sessions WHERE id=? AND state='idle'", sid, 0) == 1);

    /* The unanswered-leaf resume now owns the retry: turn 2 dispatches. */
    out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);

    db_close(db);
    printf("  PASS test_lost_submit_reparks_idle\n");
}

int main(void) {
    TEST_INIT();
    printf("test_advance_session:\n");
    test_idle_no_inbox();
    test_idle_with_inbox();
    test_idle_unanswered_user_leaf();
    test_idle_answered_leaf_noop();
    test_llm_complete_stop();
    test_tool_running_all_done();
    test_max_iterations();
    test_waiting();
    test_stale_call_reconciled_at_turn_start();
    test_open_undispatched_turn_not_stranded();
    test_parallel_subagents_never_reconciled();
    test_background_result_not_truncated();
    test_delivery_stamps_cursor();
    test_sweep_redelivers_lost_push();
    test_error_stop_reaches_parent();
    test_idle_unanswered_tool_leaf_resumes();
    test_lost_submit_reparks_idle();
    printf("All advance_session tests passed.\n");
    return 0;
}
