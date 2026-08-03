/* Cross-turn runaway guard: the consecutive-autonomous-turn streak, the
 * turn-open refusal, and the two notes it writes. */
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

static int64_t scalar(sqlite3 *db, const char *sql, int64_t sid) {
    return db_scalar_i64(db, sql, sid, -1);
}

/* The shipped fragment, run straight from the header — one place, one test. */
static int streak_of(sqlite3 *db, int64_t sid) {
    return (int)scalar(db, "SELECT " CCLAW_SQL_AUTONOMOUS_STREAK("?1") ";", sid);
}

static int leaf_role(sqlite3 *db, int64_t sid) {
    return (int)scalar(db, "SELECT role FROM entries"
                           " WHERE id=(SELECT leaf_id FROM sessions WHERE id=?1);", sid);
}

static int state_is(sqlite3 *db, int64_t sid, const char *want) {
    char *s = db_scalar_text(db, "SELECT state FROM sessions WHERE id=?1;", sid);
    int ok = s && strcmp(s, want) == 0;
    free(s);
    return ok;
}

static int notes(sqlite3 *db, int64_t sid, const char *note) {
    char sql[256];
    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM entries WHERE session_id=?1 AND role=0"
             " AND json_extract(data,'$.source')='streak_guard'"
             " AND json_extract(data,'$.note')='%s';", note);
    return (int)scalar(db, sql, sid);
}

static void set_cap(sqlite3 *db, int cap) {
    char v[16];
    snprintf(v, sizeof(v), "%d", cap);
    assert(config_set(db, "max_autonomous_turn_streak", v) == 0);
}

/* One whole turn off one inbox row: drain + dispatch, then the assistant
 * answer, then the turn-end advance. Real turn_ids (the entries_turn_ai
 * trigger mints them) and real data stamps (the drain writes them). */
static void run_turn(sqlite3 *db, int64_t sid, const char *source) {
    inbox_insert(db, sid, source, NULL, "tick");
    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);
    Message m = { .role = ROLE_ASSISTANT, .content = "ok",
                  .stop_reason = STOP_REASON_STOP };
    entry_append_with_iteration(db, sid, &m, db_next_iteration_id(db, sid));
    out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_DONE);
}

/* Streak = turns since the last human-opened one. cli and channel names are
 * human (the classification is autonomous-side); cron/spawn/agent_result are
 * not. */
static void test_streak_mixed_history(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "t", "default", -1, 0);

    run_turn(db, sid, "cli");
    assert(streak_of(db, sid) == 0);

    run_turn(db, sid, "cron");
    run_turn(db, sid, "cron");
    run_turn(db, sid, "agent_result");
    assert(streak_of(db, sid) == 3);

    /* A channel name is not in the autonomous set — it resets. */
    run_turn(db, sid, "discord");
    assert(streak_of(db, sid) == 0);

    run_turn(db, sid, "spawn");
    run_turn(db, sid, "system");
    run_turn(db, sid, "cron_error");
    assert(streak_of(db, sid) == 3);

    /* An approval notice carries a human decision — human class. */
    run_turn(db, sid, "approval");
    assert(streak_of(db, sid) == 0);

    db_close(db);
    printf("  PASS test_streak_mixed_history\n");
}

/* A role-1 entry with no data stamp is human: only the drain stamps, and
 * every autonomous writer goes through the drain. */
static void test_unstamped_user_is_human(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "t", "default", -1, 0);
    run_turn(db, sid, "cron");
    run_turn(db, sid, "cron");
    assert(streak_of(db, sid) == 2);

    Message m = { .role = ROLE_USER, .content = "typed straight in" };
    assert(entry_append_with_iteration(db, sid, &m, 0) > 0);
    assert(scalar(db, "SELECT data IS NULL FROM entries"
                      " WHERE id=(SELECT leaf_id FROM sessions WHERE id=?1);", sid) == 1);
    assert(streak_of(db, sid) == 0);

    db_close(db);
    printf("  PASS test_unstamped_user_is_human\n");
}

/* At the cap, an all-autonomous batch does not open a turn — and the rows it
 * refused stay queued. */
static void test_gate_refuses_at_cap(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "t", "default", -1, 0);
    set_cap(db, 2);
    run_turn(db, sid, "cron");
    run_turn(db, sid, "cron");

    int64_t entries_before = scalar(db, "SELECT COUNT(*) FROM entries WHERE session_id=?1;", sid);
    inbox_insert(db, sid, "cron", "nightly", "fire");
    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_NOOP);
    assert(inbox_count(db, sid) == 1);          /* durable, not consumed */
    assert(state_is(db, sid, "idle"));
    /* The one write a refusal makes is the trip marker. */
    assert(scalar(db, "SELECT COUNT(*) FROM entries WHERE session_id=?1;", sid)
           == entries_before + 1);
    assert(notes(db, sid, "tripped") == 1);
    /* Role 0: invisible to the streak SQL, which reads role 1. */
    assert(streak_of(db, sid) == 2);

    /* cap=0 turns the guard off entirely. */
    set_cap(db, 0);
    out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);
    assert(inbox_count(db, sid) == 0);

    db_close(db);
    printf("  PASS test_gate_refuses_at_cap\n");
}

/* Recovery is the reset: a human row in the batch both gets through and (by
 * construction, no extra code) zeroes the streak. */
static void test_human_row_passes_and_resets(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "t", "default", -1, 0);
    set_cap(db, 2);
    run_turn(db, sid, "cron");
    run_turn(db, sid, "cron");

    inbox_insert(db, sid, "cron", "nightly", "fire");
    assert(advance_session(db, sid, 25).action == ADVANCE_NOOP);

    /* Same queue, now with a human in it. */
    inbox_insert(db, sid, "cli", NULL, "stop that");
    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);
    assert(inbox_count(db, sid) == 0);
    Message m = { .role = ROLE_ASSISTANT, .content = "ok",
                  .stop_reason = STOP_REASON_STOP };
    entry_append_with_iteration(db, sid, &m, db_next_iteration_id(db, sid));
    assert(advance_session(db, sid, 25).action == ADVANCE_DONE);
    assert(streak_of(db, sid) == 0);

    /* And autonomous work flows again. */
    run_turn(db, sid, "cron");
    assert(streak_of(db, sid) == 1);

    db_close(db);
    printf("  PASS test_human_row_passes_and_resets\n");
}

/* Teach at ~80%: one system note in the opening batch, naming both numbers. */
static void test_warn_at_eighty_percent(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "t", "default", -1, 0);
    set_cap(db, 5);

    run_turn(db, sid, "cron");   /* turn 1 — (0+1)*5 < 20 */
    run_turn(db, sid, "cron");   /* turn 2 */
    run_turn(db, sid, "cron");   /* turn 3 */
    assert(notes(db, sid, "warn") == 0);

    run_turn(db, sid, "cron");   /* turn 4 — (3+1)*5 >= 20 */
    assert(notes(db, sid, "warn") == 1);
    char *txt = db_scalar_text(db,
        "SELECT content FROM entries WHERE session_id=?1 AND role=0"
        " AND json_extract(data,'$.note')='warn' ORDER BY id DESC LIMIT 1;", sid);
    assert(txt && strstr(txt, "autonomous turn 4 in a row (limit 5)"));
    free(txt);

    /* One per qualifying turn, and the note rides the turn it opened — no new
     * turn_id, so it never inflates the streak it warns about. */
    assert(streak_of(db, sid) == 4);
    run_turn(db, sid, "cron");   /* turn 5 */
    assert(notes(db, sid, "warn") == 2);
    assert(streak_of(db, sid) == 5);

    /* Cap reached: turn 6 is refused, not warned. */
    inbox_insert(db, sid, "cron", NULL, "fire");
    assert(advance_session(db, sid, 25).action == ADVANCE_NOOP);
    assert(notes(db, sid, "warn") == 2);
    assert(notes(db, sid, "tripped") == 1);

    db_close(db);
    printf("  PASS test_warn_at_eighty_percent\n");
}

/* One chat notice per trip — derived from the marker entry, not a column. A
 * human turn in between starts a fresh trip, which warns again. */
static void test_trip_notice_once_per_trip(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "t", "default", -1, 0);
    assert(sqlite3_exec(db, "UPDATE sessions SET channel_name='discord',"
                            " chat_id='42';", NULL, NULL, NULL) == SQLITE_OK);
    set_cap(db, 2);
    run_turn(db, sid, "cron");
    run_turn(db, sid, "cron");

    inbox_insert(db, sid, "cron", NULL, "fire");
    assert(advance_session(db, sid, 25).action == ADVANCE_NOOP);
    assert(scalar(db, "SELECT COUNT(*) FROM channel_outbox WHERE session_id=?1 AND payload LIKE '%autonomous-turn limit%';", sid) == 1);
    char *pay = db_scalar_text(db,
        "SELECT json_extract(payload,'$.text') FROM channel_outbox"
        " WHERE session_id=?1 AND payload LIKE '%autonomous-turn limit%';", sid);
    assert(pay && strstr(pay, "autonomous-turn limit (2 consecutive turns"));
    free(pay);

    /* Second and third refusals of the same trip say nothing more. */
    inbox_insert(db, sid, "cron", NULL, "fire again");
    assert(advance_session(db, sid, 25).action == ADVANCE_NOOP);
    assert(advance_session(db, sid, 25).action == ADVANCE_NOOP);
    assert(scalar(db, "SELECT COUNT(*) FROM channel_outbox WHERE session_id=?1 AND payload LIKE '%autonomous-turn limit%';", sid) == 1);
    assert(notes(db, sid, "tripped") == 1);

    /* A human replies: the queue drains, the streak clears. */
    inbox_insert(db, sid, "discord", NULL, "hello?");
    assert(advance_session(db, sid, 25).action == ADVANCE_DISPATCH_LLM);
    Message m = { .role = ROLE_ASSISTANT, .content = "ok",
                  .stop_reason = STOP_REASON_STOP };
    entry_append_with_iteration(db, sid, &m, db_next_iteration_id(db, sid));
    assert(advance_session(db, sid, 25).action == ADVANCE_DONE);

    /* Fresh trip → fresh notice. */
    run_turn(db, sid, "cron");
    run_turn(db, sid, "cron");
    inbox_insert(db, sid, "cron", NULL, "fire");
    assert(advance_session(db, sid, 25).action == ADVANCE_NOOP);
    assert(scalar(db, "SELECT COUNT(*) FROM channel_outbox WHERE session_id=?1 AND payload LIKE '%autonomous-turn limit%';", sid) == 2);
    assert(notes(db, sid, "tripped") == 2);

    db_close(db);
    printf("  PASS test_trip_notice_once_per_trip\n");
}

/* The marker is a role-0 leaf — a legal resting place. It must not look like
 * the unanswered leaf (role 1/3) that advance_session resumes. */
static void test_tripped_leaf_does_not_resume(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "t", "default", -1, 0);
    set_cap(db, 1);
    run_turn(db, sid, "cron");

    inbox_insert(db, sid, "cron", NULL, "fire");
    assert(advance_session(db, sid, 25).action == ADVANCE_NOOP);
    assert(leaf_role(db, sid) == 0);

    /* Empty the queue: nothing left to gate, and nothing to resume either. */
    assert(sqlite3_exec(db, "UPDATE inbox SET consumed=1;", NULL, NULL, NULL) == SQLITE_OK);
    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_NOOP);
    assert(state_is(db, sid, "idle"));
    assert(leaf_role(db, sid) == 0);

    db_close(db);
    printf("  PASS test_tripped_leaf_does_not_resume\n");
}

/* The compacting branch is the idle branch's twin — it consumes the inbox and
 * flips to llm_running too, so it takes the same gate. */
static void test_compacting_twin_gated(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "t", "default", -1, 0);
    set_cap(db, 2);
    run_turn(db, sid, "cron");
    run_turn(db, sid, "cron");

    inbox_insert(db, sid, "cron", NULL, "fire");
    assert(session_set_state(db, sid, "compacting") == 0);
    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_NOOP);
    assert(state_is(db, sid, "idle"));   /* compaction still ends */
    assert(inbox_count(db, sid) == 1);
    assert(notes(db, sid, "tripped") == 1);

    /* Human row in the batch: the same branch opens the turn. */
    inbox_insert(db, sid, "cli", NULL, "hi");
    assert(session_set_state(db, sid, "compacting") == 0);
    out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);
    assert(state_is(db, sid, "llm_running"));
    assert(inbox_count(db, sid) == 0);

    db_close(db);
    printf("  PASS test_compacting_twin_gated\n");
}

int main(void) {
    TEST_INIT();
    printf("test_streak_guard:\n");
    test_streak_mixed_history();
    test_unstamped_user_is_human();
    test_gate_refuses_at_cap();
    test_human_row_passes_and_resets();
    test_warn_at_eighty_percent();
    test_trip_notice_once_per_trip();
    test_tripped_leaf_does_not_resume();
    test_compacting_twin_gated();
    printf("All streak guard tests passed.\n");
    return 0;
}
