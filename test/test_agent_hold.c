/* The quiesce lease (agents.hold_until/hold_holder) and the one gate that
 * honours it: while an agent is held, advance_session must not open a NEW
 * turn for any of its sessions. Queued work stays queued — nothing is lost,
 * delivery is deferred — and an expired lease self-heals. */
#include "advance.h"
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

static void exec_ok(sqlite3 *db, const char *sql) {
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
}

static int state_is(sqlite3 *db, int64_t sid, const char *want) {
    char *s = db_scalar_text(db, "SELECT state FROM sessions WHERE id=?1;", sid);
    int ok = s && strcmp(s, want) == 0;
    free(s);
    return ok;
}

/* Held: the turn does not open, the inbox row survives, nothing is written. */
static void test_held_agent_does_not_open_a_turn(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "t", "default", -1, 0);
    inbox_insert(db, sid, "cron", NULL, "fire");
    int64_t entries_before = db_scalar_i64(db,
        "SELECT COUNT(*) FROM entries WHERE session_id=?1;", sid, -1);

    assert(agent_hold_acquire(db, "default", 60, "cli:test") == 0);

    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_NOOP);
    assert(out.deferred == 1);
    assert(inbox_count(db, sid) == 1);
    assert(state_is(db, sid, "idle"));
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM entries WHERE session_id=?1;",
                         sid, -1) == entries_before);

    /* Released: the same queued work opens the turn, unchanged. */
    assert(agent_hold_release(db, "default", "cli:test") == 0);
    out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);
    assert(out.deferred == 0);
    assert(inbox_count(db, sid) == 0);

    db_close(db);
    printf("  PASS test_held_agent_does_not_open_a_turn\n");
}

/* A holder that died mid-rename must not wedge the agent forever: the lease
 * is a deadline, and a past deadline is no lease at all. */
static void test_expired_lease_promotes(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "t", "default", -1, 0);
    inbox_insert(db, sid, "cron", NULL, "fire");

    exec_ok(db, "UPDATE agents SET hold_until=unixepoch()-1,"
                " hold_holder='cli:ghost' WHERE name='default';");
    AdvanceOutput out = advance_session(db, sid, 25);
    assert(out.action == ADVANCE_DISPATCH_LLM);
    assert(inbox_count(db, sid) == 0);

    db_close(db);
    printf("  PASS test_expired_lease_promotes\n");
}

/* The lease is per-agent, not global: another agent's sessions run normally
 * while one is held. */
static void test_hold_is_scoped_to_the_agent(void) {
    sqlite3 *db = open_seeded();
    test_seed_agent(db, "other");
    int64_t held = session_create(db, "h", "default", -1, 0);
    int64_t free_sid = session_create(db, "f", "other", -1, 0);
    inbox_insert(db, held, "cron", NULL, "fire");
    inbox_insert(db, free_sid, "cron", NULL, "fire");

    assert(agent_hold_acquire(db, "default", 60, "cli:test") == 0);
    assert(advance_session(db, held, 25).action == ADVANCE_NOOP);
    assert(advance_session(db, free_sid, 25).action == ADVANCE_DISPATCH_LLM);

    db_close(db);
    printf("  PASS test_hold_is_scoped_to_the_agent\n");
}

/* Two operators racing: exactly one gets the lease. */
static void test_lease_contention(void) {
    sqlite3 *db = open_seeded();
    assert(agent_hold_acquire(db, "default", 60, "cli:100") == 0);
    assert(agent_hold_acquire(db, "default", 60, "cli:200") == -1);
    /* The loser's hold_holder never landed — the winner still owns it. */
    char *who = db_scalar_text(db,
        "SELECT hold_holder FROM agents WHERE name='default';", 0);
    assert(who && strcmp(who, "cli:100") == 0);
    free(who);
    db_close(db);
    printf("  PASS test_lease_contention\n");
}

int main(void) {
    TEST_INIT();
    printf("test_agent_hold:\n");
    test_held_agent_does_not_open_a_turn();
    test_expired_lease_promotes();
    test_hold_is_scoped_to_the_agent();
    test_lease_contention();
    printf("All agent hold tests passed.\n");
    return 0;
}
