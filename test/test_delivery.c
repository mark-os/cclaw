/* Delivery edges (specs/delivery.md): quiescence, policy semantics, the
 * session-5 regression (one notice per child, the real answer), blocking
 * one-shot + standing-edge interplay, inheritance, and the pause fail-notify. */
#include "advance.h"
#include "db.h"
#include "test_util.h"
#include "tool_agent.h"
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

static void set_cfg(sqlite3 *db, const char *key, const char *val) {
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO config(key, value) VALUES(?1, ?2)"
        " ON CONFLICT(key) DO UPDATE SET value=excluded.value;",
        -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, val, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
}

/* Launch through the real handler; returns the new child's session id. */
static int64_t launch(sqlite3 *db, int64_t launcher, const char *args,
                      const char *call_id) {
    AgentLaunchCtx ctx = { .db = db, .session_id = launcher,
                           .current_tool_call_id = (char *)call_id };
    char *r = tool_launch_agent_handler(args, &ctx, &(int){0});
    if (call_id) assert(r == NULL);          /* blocking parks */
    else { assert(r && strncmp(r, "error:", 6) != 0); free(r); }
    return scalar(db, "SELECT MAX(id) FROM sessions WHERE parent_session_id=?;",
                  launcher);
}

/* The dispatcher's half of a blocking launch: the claimed tool_calls row. */
static void claim_call(sqlite3 *db, int64_t parent, const char *call_id) {
    char ins[256];
    snprintf(ins, sizeof(ins),
        "INSERT INTO tool_calls(session_id, entry_id, call_id, name, status)"
        " VALUES(%lld, 1, '%s', 'launch_agent', 'running');",
        (long long)parent, call_id);
    assert(sqlite3_exec(db, ins, NULL, NULL, NULL) == SQLITE_OK);
}

/* One full turn the way the daemon runs it: drain the inbox (idle →
 * llm_running), land the assistant answer, advance to the boundary. */
static AdvanceResult run_turn(sqlite3 *db, int64_t sid, const char *answer) {
    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);
    Message m = { .role = ROLE_ASSISTANT, .content = (char *)answer,
                  .stop_reason = STOP_REASON_STOP };
    entry_append_with_iteration(db, sid, &m, db_next_iteration_id(db, sid));
    return advance_session(db, sid, 25).action;
}

static int64_t notices(sqlite3 *db, int64_t sid) {
    return scalar(db, "SELECT COUNT(*) FROM inbox WHERE session_id=?"
                      " AND source='agent_result';", sid);
}

/* ── Quiescence: the four conditions over a deep tree ─────────────────── */

static void test_quiescence_deep_tree(void) {
    sqlite3 *db = open_seeded();
    int64_t root = session_create(db, "root", "default", -1, 0);
    int64_t mid = session_create(db, "mid", "default", root, 1);
    int64_t leaf = session_create(db, "leaf", "default", mid, 2);

    assert(session_subtree_quiescent(db, root) == 1);

    /* 1: a non-idle session anywhere in the subtree. */
    session_set_state(db, leaf, "llm_running");
    assert(session_subtree_quiescent(db, root) == 0);
    assert(session_subtree_quiescent(db, mid) == 0);
    assert(session_subtree_quiescent(db, leaf) == 0);
    session_set_state(db, leaf, "idle");
    assert(session_subtree_quiescent(db, root) == 1);

    /* 2: the unconsumed-inbox interim state — everyone idle while a result
     * sits in the middle session's inbox is NOT quiescent. */
    int64_t row = inbox_insert(db, mid, "agent_result", NULL, "grandchild done");
    assert(row > 0);
    assert(session_subtree_quiescent(db, root) == 0);
    assert(session_subtree_quiescent(db, leaf) == 1);   /* leaf subtree clean */
    assert(sqlite3_exec(db, "UPDATE inbox SET consumed=1;", NULL, NULL, NULL)
           == SQLITE_OK);
    assert(session_subtree_quiescent(db, root) == 1);

    /* 3: an in-flight llm_job. */
    assert(sqlite3_exec(db,
        "INSERT INTO llm_jobs(session_id, agent_name) SELECT id, 'default'"
        " FROM sessions WHERE name='leaf';", NULL, NULL, NULL) == SQLITE_OK);
    assert(session_subtree_quiescent(db, root) == 0);
    assert(sqlite3_exec(db, "DELETE FROM llm_jobs;", NULL, NULL, NULL) == SQLITE_OK);

    /* 4: a live tool_call. */
    claim_call(db, leaf, "call_x");
    assert(session_subtree_quiescent(db, root) == 0);
    assert(sqlite3_exec(db, "UPDATE tool_calls SET status='done';",
                        NULL, NULL, NULL) == SQLITE_OK);
    assert(session_subtree_quiescent(db, root) == 1);

    db_close(db);
    printf("  PASS test_quiescence_deep_tree\n");
}

/* ── The session-5 regression: a 2×2 background tree, one notice per child ── */

static void test_session5_one_notice_per_child(void) {
    sqlite3 *db = open_seeded();
    int64_t root = session_create(db, "root", "default", -1, 0);

    int64_t kid[2], gkid[2][2];
    for (int i = 0; i < 2; i++)
        kid[i] = launch(db, root, "{\"task\":\"delegate\",\"background\":true}", NULL);
    assert(kid[0] > 0 && kid[1] > kid[0]);

    for (int i = 0; i < 2; i++) {
        /* Child turn 1: drain the spawn task, launch two background
         * grandchildren mid-turn, close with interim prose. */
        AdvanceOutput out = advance_session(db, kid[i], 25);
        assert(out.action == ADVANCE_DISPATCH_LLM);
        for (int j = 0; j < 2; j++)
            gkid[i][j] = launch(db, kid[i],
                                "{\"task\":\"dig\",\"background\":true}", NULL);
        Message m = { .role = ROLE_ASSISTANT,
                      .content = "delegated to two grandchildren",
                      .stop_reason = STOP_REASON_STOP };
        entry_append_with_iteration(db, kid[i], &m, db_next_iteration_id(db, kid[i]));
        assert(advance_session(db, kid[i], 25).action == ADVANCE_DONE);
    }

    /* THE regression: prod session 5 pushed "completed" here. Under the
     * quiescent default the interim boundary ships nothing. */
    assert(notices(db, root) == 0);

    /* Grandchildren finish: each notifies its own parent (subtree settled). */
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 2; j++)
            assert(run_turn(db, gkid[i][j], "dug up a fact") == ADVANCE_DONE);
        assert(notices(db, kid[i]) == 2);
        assert(notices(db, root) == 0);       /* children haven't settled yet */
    }

    /* Children consume both results and settle: exactly one notice each,
     * carrying the final answer, labeled truthfully. */
    for (int i = 0; i < 2; i++)
        assert(run_turn(db, kid[i], "both facts combined") == ADVANCE_DONE);
    assert(notices(db, root) == 2);
    assert(scalar(db, "SELECT COUNT(*) FROM inbox WHERE session_id=?"
                      " AND source='agent_result'"
                      " AND payload = 'Sub-agent completed: both facts combined';",
                  root) == 2);

    db_close(db);
    printf("  PASS test_session5_one_notice_per_child\n");
}

/* ── Blocking: one-shot fires once, standing edge ships later work ────── */

static void test_blocking_standing_no_duplicate(void) {
    sqlite3 *db = open_seeded();
    int64_t parent = session_create(db, "parent", "default", -1, 0);
    claim_call(db, parent, "call_1");
    session_set_state(db, parent, "llm_running");
    session_set_state(db, parent, "tool_running");
    int64_t child = launch(db, parent, "{\"task\":\"work\"}", "call_1");
    assert(child > 0);
    assert(scalar(db, "SELECT COUNT(*) FROM delivery_edges WHERE session_id=?;",
                  child) == 2);               /* standing + one-shot */

    assert(run_turn(db, child, "answer one") == ADVANCE_DONE);
    /* The one-shot resolved the turn-join: a role-3 result, no inbox notice,
     * edge gone, standing cursor advanced in the same transaction. */
    assert(scalar(db, "SELECT COUNT(*) FROM entries WHERE session_id=? AND role=3"
                      " AND content='answer one';", parent) == 1);
    assert(notices(db, parent) == 0);
    assert(scalar(db, "SELECT COUNT(*) FROM delivery_edges WHERE session_id=?"
                      " AND one_shot=1;", child) == 0);
    assert(scalar(db, "SELECT cursor FROM delivery_edges WHERE session_id=?"
                      " AND target_kind='parent';", child) ==
           scalar(db, "SELECT leaf_id FROM sessions WHERE id=?;", child));
    /* Nothing owed → the sweep is quiet. */
    assert(advance_sweep_undelivered(db) == 0);

    /* Post-unblock work ships via the standing edge — the closed
     * lost-final-answer gap — with no duplicate of the one-shot payload. */
    inbox_insert(db, child, "cli", NULL, "follow up");
    assert(run_turn(db, child, "answer two") == ADVANCE_DONE);
    assert(notices(db, parent) == 1);
    assert(scalar(db, "SELECT COUNT(*) FROM inbox WHERE session_id=?"
                      " AND payload = 'Sub-agent completed: answer two';",
                  parent) == 1);

    db_close(db);
    printf("  PASS test_blocking_standing_no_duplicate\n");
}

/* ── Blocking refuses multi-payload policies, with feedback ───────────── */

static void test_blocking_policy_refusal(void) {
    sqlite3 *db = open_seeded();
    int64_t parent = session_create(db, "parent", "default", -1, 0);

    const char *bad[] = {
        "{\"task\":\"t\",\"delivery\":\"explicit\"}",
        "{\"task\":\"t\",\"delivery\":\"iteration\"}",
    };
    for (int i = 0; i < 2; i++) {
        AgentLaunchCtx ctx = { .db = db, .session_id = parent,
                               .current_tool_call_id = "call_x" };
        char *r = tool_launch_agent_handler(bad[i], &ctx, &(int){0});
        assert(r && strstr(r, "single-report delivery policy"));
        free(r);
    }
    AgentLaunchCtx ctx = { .db = db, .session_id = parent };
    char *r = tool_launch_agent_handler("{\"task\":\"t\",\"delivery\":\"chatty\"}",
                                        &ctx, &(int){0});
    assert(r && strstr(r, "unknown delivery policy"));
    free(r);
    assert(scalar(db, "SELECT COUNT(*) FROM sessions WHERE parent_session_id=?;",
                  parent) == 0);

    /* The same policies are legal in background. */
    int64_t child = launch(db, parent,
        "{\"task\":\"t\",\"background\":true,\"delivery\":\"explicit\"}", NULL);
    char *pol = db_scalar_text(db,
        "SELECT policy FROM delivery_edges WHERE session_id=?", child);
    assert(pol && strcmp(pol, "explicit") == 0);
    free(pol);

    db_close(db);
    printf("  PASS test_blocking_policy_refusal\n");
}

/* ── Inheritance: the launcher's policy seeds the child; explicit doesn't ── */

static void test_policy_inheritance(void) {
    sqlite3 *db = open_seeded();
    int64_t root = session_create(db, "root", "default", -1, 0);

    /* Explicit arg freezes 'turn'; the grandchild inherits it. */
    int64_t child = launch(db, root,
        "{\"task\":\"t\",\"background\":true,\"delivery\":\"turn\"}", NULL);
    int64_t gchild = launch(db, child, "{\"task\":\"t\",\"background\":true}", NULL);
    char *pol = db_scalar_text(db,
        "SELECT policy FROM delivery_edges WHERE session_id=?", gchild);
    assert(pol && strcmp(pol, "turn") == 0);
    free(pol);

    /* An 'explicit' launcher has nothing inheritable: registry default. */
    int64_t mute = launch(db, root,
        "{\"task\":\"t\",\"background\":true,\"delivery\":\"explicit\"}", NULL);
    int64_t loud = launch(db, mute, "{\"task\":\"t\",\"background\":true}", NULL);
    pol = db_scalar_text(db,
        "SELECT policy FROM delivery_edges WHERE session_id=?", loud);
    assert(pol && strcmp(pol, "quiescent") == 0);
    free(pol);

    db_close(db);
    printf("  PASS test_policy_inheritance\n");
}

/* ── digest: prose since the cursor, concatenated, windowed per delivery ── */

static void test_digest_window(void) {
    sqlite3 *db = open_seeded();
    int64_t parent = session_create(db, "parent", "default", -1, 0);
    int64_t child = launch(db, parent,
        "{\"task\":\"t\",\"background\":true,\"delivery\":\"digest\"}", NULL);

    /* Turn 1: interim prose (tool_use commentary) plus the final message. */
    AdvanceOutput out = advance_session(db, child, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);
    entry_append_typed(db, child, 1, "assistant_message", 0, "step one",
                       NULL, NULL, 0, STOP_REASON_TOOL_USE, NULL, 0, 0, 0);
    Message m = { .role = ROLE_ASSISTANT, .content = "done one",
                  .stop_reason = STOP_REASON_STOP };
    entry_append_with_iteration(db, child, &m, 2);
    assert(advance_session(db, child, 25).action == ADVANCE_DONE);
    assert(scalar(db, "SELECT COUNT(*) FROM inbox WHERE session_id=? AND payload ="
        " 'Sub-agent completed: step one' || char(10)||char(10)||'---'||char(10)||char(10) || 'done one';",
        parent) == 1);

    /* Turn 2's digest starts after the cursor: only the new prose. */
    inbox_insert(db, child, "cli", NULL, "again");
    assert(run_turn(db, child, "done two") == ADVANCE_DONE);
    assert(scalar(db, "SELECT COUNT(*) FROM inbox WHERE session_id=?"
                      " AND payload = 'Sub-agent completed: done two';",
                  parent) == 1);

    db_close(db);
    printf("  PASS test_digest_window\n");
}

/* ── turn: the old standard ships every boundary, labeled truthfully ───── */

static void test_turn_policy_truthful_labels(void) {
    sqlite3 *db = open_seeded();
    int64_t parent = session_create(db, "parent", "default", -1, 0);
    int64_t child = launch(db, parent,
        "{\"task\":\"t\",\"background\":true,\"delivery\":\"turn\"}", NULL);

    /* Turn 1 launches a background grandchild and ends while it runs: the
     * push happens (turn semantics) but says update, not completed. */
    AdvanceOutput out = advance_session(db, child, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);
    int64_t gchild = launch(db, child, "{\"task\":\"dig\",\"background\":true}", NULL);
    Message m = { .role = ROLE_ASSISTANT, .content = "delegated",
                  .stop_reason = STOP_REASON_STOP };
    entry_append_with_iteration(db, child, &m, db_next_iteration_id(db, child));
    assert(advance_session(db, child, 25).action == ADVANCE_DONE);
    assert(scalar(db, "SELECT COUNT(*) FROM inbox WHERE session_id=?"
                      " AND payload = 'Sub-agent update: delegated';", parent) == 1);

    /* Subtree settles → the next boundary says completed. */
    assert(run_turn(db, gchild, "fact") == ADVANCE_DONE);
    assert(run_turn(db, child, "final") == ADVANCE_DONE);
    assert(scalar(db, "SELECT COUNT(*) FROM inbox WHERE session_id=?"
                      " AND payload = 'Sub-agent completed: final';", parent) == 1);

    db_close(db);
    printf("  PASS test_turn_policy_truthful_labels\n");
}

/* ── F2: the tag comes from the child's stop_reason, not its prose ─────── */

static void test_child_outcome_truncated(void) {
    sqlite3 *db = open_seeded();
    int64_t parent = session_create(db, "parent", "default", -1, 0);
    int64_t child = launch(db, parent, "{\"task\":\"t\",\"background\":true}", NULL);

    /* The child claims success in prose but stopped at the output cap. */
    AdvanceOutput out = advance_session(db, child, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);
    Message m = { .role = ROLE_ASSISTANT, .content = "all done, task complete",
                  .stop_reason = STOP_REASON_LENGTH };
    entry_append_with_iteration(db, child, &m, db_next_iteration_id(db, child));
    assert(advance_session(db, child, 25).action == ADVANCE_DONE);

    assert(session_final_truncated(db, child) == 1);
    assert(scalar(db, "SELECT COUNT(*) FROM inbox WHERE session_id=?"
                      " AND payload LIKE 'Sub-agent truncated: %';", parent) == 1);
    assert(scalar(db, "SELECT COUNT(*) FROM inbox WHERE session_id=?"
                      " AND payload LIKE 'Sub-agent completed: %';", parent) == 0);

    db_close(db);
    printf("  PASS test_child_outcome_truncated\n");
}

/* ── The sweep delivers a quiescent hold that settled silently ─────────── */

static void test_sweep_delivers_silent_settle(void) {
    sqlite3 *db = open_seeded();
    int64_t root = session_create(db, "root", "default", -1, 0);
    int64_t child = launch(db, root, "{\"task\":\"t\",\"background\":true}", NULL);

    /* Child's turn launches an 'explicit' grandchild and ends: held. */
    AdvanceOutput out = advance_session(db, child, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);
    int64_t gchild = launch(db, child,
        "{\"task\":\"quiet work\",\"background\":true,\"delivery\":\"explicit\"}",
        NULL);
    Message m = { .role = ROLE_ASSISTANT, .content = "the answer",
                  .stop_reason = STOP_REASON_STOP };
    entry_append_with_iteration(db, child, &m, db_next_iteration_id(db, child));
    assert(advance_session(db, child, 25).action == ADVANCE_DONE);
    assert(notices(db, root) == 0);

    /* The explicit grandchild finishes without a sound — no boundary ever
     * fires on the child again. Only the sweep can see the settle. */
    assert(run_turn(db, gchild, "unreported") == ADVANCE_DONE);
    assert(notices(db, child) == 0);
    assert(notices(db, root) == 0);

    assert(advance_sweep_undelivered(db) == 1);
    assert(scalar(db, "SELECT COUNT(*) FROM inbox WHERE session_id=?"
                      " AND payload = 'Sub-agent completed: the answer';",
                  root) == 1);
    assert(advance_sweep_undelivered(db) == 0);   /* idempotent by the cursor */

    db_close(db);
    printf("  PASS test_sweep_delivers_silent_settle\n");
}

/* ── Streak pause fail-notifies a parked blocking waiter ──────────────── */

static void test_streak_pause_fail_notify(void) {
    sqlite3 *db = open_seeded();
    set_cfg(db, "max_autonomous_turn_streak", "1");
    int64_t parent = session_create(db, "parent", "default", -1, 0);
    claim_call(db, parent, "call_p");
    session_set_state(db, parent, "llm_running");
    session_set_state(db, parent, "tool_running");
    int64_t child = launch(db, parent, "{\"task\":\"work\"}", "call_p");

    /* Turn 1 (spawn = autonomous) launches a grandchild that never finishes,
     * so the one-shot stays parked on a subtree that will not settle. */
    AdvanceOutput out = advance_session(db, child, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);
    launch(db, child, "{\"task\":\"forever\",\"background\":true}", NULL);
    Message m = { .role = ROLE_ASSISTANT, .content = "working",
                  .stop_reason = STOP_REASON_STOP };
    entry_append_with_iteration(db, child, &m, db_next_iteration_id(db, child));
    assert(advance_session(db, child, 25).action == ADVANCE_DONE);
    assert(scalar(db, "SELECT COUNT(*) FROM entries WHERE session_id=? AND role=3;",
                  parent) == 0);   /* held, parent still parked */

    /* The next autonomous open trips the guard: the parked waiter must get
     * an error result now — a paused session never settles. */
    inbox_insert(db, child, "cron", NULL, "fire");
    assert(advance_session(db, child, 25).action == ADVANCE_NOOP);
    assert(scalar(db, "SELECT COUNT(*) FROM entries WHERE session_id=? AND role=3"
                      " AND is_error=1 AND content LIKE '%autonomy guard%';",
                  parent) == 1);
    assert(scalar(db, "SELECT COUNT(*) FROM tool_calls WHERE session_id=?"
                      " AND status='done';", parent) == 1);
    assert(scalar(db, "SELECT COUNT(*) FROM delivery_edges WHERE session_id=?"
                      " AND one_shot=1;", child) == 0);

    db_close(db);
    printf("  PASS test_streak_pause_fail_notify\n");
}

/* ── Channel edge: lazy freeze from the route, coalescing, explicit ────── */

static void test_channel_edge_semantics(void) {
    sqlite3 *db = open_seeded();

    /* Unrouted chat-bound session (cron 'new' shape): freezes quiescent.
     * A follow-up already queued at turn end coalesces — only the final
     * answer ships. */
    int64_t sid = session_create(db, "chat", "default", -1, 0);
    assert(sqlite3_exec(db, "UPDATE sessions SET channel_name='discord',"
                            " chat_id='42' WHERE name='chat';",
                        NULL, NULL, NULL) == SQLITE_OK);
    inbox_insert(db, sid, "discord", NULL, "q1");
    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);
    inbox_insert(db, sid, "discord", NULL, "q2");   /* queued mid-turn */
    Message m = { .role = ROLE_ASSISTANT, .content = "a1",
                  .stop_reason = STOP_REASON_STOP };
    entry_append_with_iteration(db, sid, &m, db_next_iteration_id(db, sid));
    assert(advance_session(db, sid, 25).action == ADVANCE_DONE);
    assert(scalar(db, "SELECT COUNT(*) FROM channel_outbox WHERE session_id=?;",
                  sid) == 0);                       /* held: q2 pending */
    char *pol = db_scalar_text(db,
        "SELECT policy FROM delivery_edges WHERE session_id=?"
        " AND target_kind='channel';", sid);
    assert(pol && strcmp(pol, "quiescent") == 0);   /* 'auto' alias, frozen */
    free(pol);

    assert(run_turn(db, sid, "a2 covering both") == ADVANCE_DONE);
    assert(scalar(db, "SELECT COUNT(*) FROM channel_outbox WHERE session_id=?;",
                  sid) == 1);
    char *text = db_scalar_text(db,
        "SELECT json_extract(payload,'$.text') FROM channel_outbox"
        " WHERE session_id=?;", sid);
    assert(text && strcmp(text, "a2 covering both") == 0);
    free(text);

    /* An 'explicit' route freezes an edge that never auto-ships. */
    int64_t quiet = session_create(db, "quiet", "default", -1, 0);
    char rsql[256];
    snprintf(rsql, sizeof(rsql),
        "UPDATE sessions SET channel_name='discord', chat_id='43' WHERE id=%lld;"
        "INSERT INTO channel_routes(channel_name, chat_id, session_id,"
        " delivery_mode) VALUES('discord','43',%lld,'explicit');",
        (long long)quiet, (long long)quiet);
    assert(sqlite3_exec(db, rsql, NULL, NULL, NULL) == SQLITE_OK);
    inbox_insert(db, quiet, "discord", NULL, "psst");
    assert(run_turn(db, quiet, "silent answer") == ADVANCE_DONE);
    assert(scalar(db, "SELECT COUNT(*) FROM channel_outbox WHERE session_id=?;",
                  quiet) == 0);
    pol = db_scalar_text(db,
        "SELECT policy FROM delivery_edges WHERE session_id=?"
        " AND target_kind='channel';", quiet);
    assert(pol && strcmp(pol, "explicit") == 0);
    free(pol);
    assert(advance_sweep_undelivered(db) == 0);     /* sweep skips explicit */

    db_close(db);
    printf("  PASS test_channel_edge_semantics\n");
}

int main(void) {
    TEST_INIT();
    printf("test_delivery:\n");
    test_quiescence_deep_tree();
    test_session5_one_notice_per_child();
    test_blocking_standing_no_duplicate();
    test_blocking_policy_refusal();
    test_policy_inheritance();
    test_digest_window();
    test_turn_policy_truthful_labels();
    test_child_outcome_truncated();
    test_sweep_delivers_silent_settle();
    test_streak_pause_fail_notify();
    test_channel_edge_semantics();
    printf("All delivery tests passed.\n");
    return 0;
}
