/* Test session_set_state concurrency guard */
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Each row walks one fresh (idle) session through its steps; every step
 * names the result session_set_state must give. A busy state moves to
 * another busy state, a waiting state is reachable only from a busy one, and
 * idle -> idle is refused. Deleted states (awaiting_agent, compacting) and
 * the legacy "running" are unknown names now. */
static void test_transitions(void) {
    static const struct {
        const char *name;
        struct { const char *state; int rc; } steps[6];
    } cases[] = {
        { "idle <-> busy", { {"llm_running", 0}, {"idle", 0},
                             {"tool_running", 0}, {"idle", 0} } },
        { "busy -> busy",  { {"llm_running", 0}, {"tool_running", 0},
                             {"llm_running", 0}, {"idle", 0} } },
        { "idle -> idle",              { {"idle", -1} } },
        { "idle -> error",             { {"error", -1} } },
        { "idle -> awaiting_approval", { {"awaiting_approval", -1} } },
        { "unknown names",             { {"awaiting_agent", -1}, {"compacting", -1},
                                         {"running", -1} } },
        { "approval then idle",  { {"llm_running", 0}, {"tool_running", 0},
                                   {"awaiting_approval", 0}, {"idle", 0} } },
        /* approval resolved: the turn resumes */
        { "approval then resume", { {"llm_running", 0}, {"awaiting_approval", 0},
                                    {"tool_running", 0}, {"idle", 0} } },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        printf("    %s\n", cases[i].name);
        sqlite3 *db = test_db_open(":memory:");
        assert(db);
        int64_t sid = session_create(db, "t", NULL, -1, 0);
        assert(sid > 0);
        for (int j = 0; j < 6 && cases[i].steps[j].state; j++)
            assert(session_set_state(db, sid, cases[i].steps[j].state) == cases[i].steps[j].rc);
        db_close(db);
    }
}

/* Two different sessions can both be busy: the guard is per session. */
static void test_concurrent_acquire(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t s1 = session_create(db, "a", NULL, -1, 0);
    int64_t s2 = session_create(db, "b", NULL, -1, 0);
    assert(s1 > 0 && s2 > 0);
    assert(session_set_state(db, s1, "llm_running") == 0);
    assert(session_set_state(db, s2, "llm_running") == 0);
    db_close(db);
}

static void test_turn_iteration(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "t", NULL, -1, 0);
    assert(sid > 0);

    /* Starts at 0 */
    assert((int)db_scalar_i64(db, "SELECT turn_iteration FROM sessions WHERE id=?", sid, -1) == 0);

    /* Set to 5 */
    assert(session_set_iteration(db, sid, 5) == 0);
    assert((int)db_scalar_i64(db, "SELECT turn_iteration FROM sessions WHERE id=?", sid, -1) == 5);

    /* Bump returns new value */
    assert(session_bump_iteration(db, sid) == 6);
    assert((int)db_scalar_i64(db, "SELECT turn_iteration FROM sessions WHERE id=?", sid, -1) == 6);

    /* Transition to idle resets iteration */
    assert(session_set_state(db, sid, "llm_running") == 0);
    assert(session_set_iteration(db, sid, 10) == 0);
    assert(session_set_state(db, sid, "idle") == 0);
    assert((int)db_scalar_i64(db, "SELECT turn_iteration FROM sessions WHERE id=?", sid, -1) == 0);

    db_close(db);
}

/* H1/H4/H5: startup crash recovery resets every transient/waiting state to
 * idle, zeroes turn_iteration, and reconciles orphaned pending tool_calls. */
static void force_state(sqlite3 *db, int64_t sid, const char *state, int iter) {
    char sql[160];
    snprintf(sql, sizeof(sql),
        "UPDATE sessions SET state='%s', turn_iteration=%d WHERE id=%lld;",
        state, iter, (long long)sid);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
}

static void test_startup_recovery(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);

    /* One session stuck in each non-idle state a crash can strand. */
    const char *stuck[] = {"llm_running","tool_running","settling",
                           "awaiting_approval","rate_limited"};
    const int n_stuck = (int)(sizeof(stuck) / sizeof(stuck[0]));
    int64_t ids[sizeof(stuck) / sizeof(stuck[0])];
    for (int i = 0; i < n_stuck; i++) {
        ids[i] = session_create(db, "t", NULL, -1, 0);
        assert(ids[i] > 0);
        force_state(db, ids[i], stuck[i], 7 /* non-zero iteration */);
    }
    /* A control session left idle must be untouched. */
    int64_t idle_id = session_create(db, "t", NULL, -1, 0);
    force_state(db, idle_id, "idle", 0);

    /* An orphaned pending tool_call: fork happened, result never written. */
    int64_t tc_sid = ids[1]; /* the tool_running one */
    assert(sqlite3_exec(db,
        "INSERT INTO tool_calls (session_id, entry_id, call_id, name, status)"
        " VALUES ((SELECT id FROM sessions WHERE state='tool_running' LIMIT 1),"
        " 1, 'call_orphan', 'shell_exec', 'pending');",
        NULL, NULL, NULL) == SQLITE_OK);

    /* An orphaned *running* tool_call: a forked tool or sub-agent was in flight
     * (the async state used for parallel dispatch) when we crashed. Recovery
     * must reconcile it exactly like a pending orphan, or the assistant turn is
     * left with an unanswered tool_call. */
    char insrun[256];
    snprintf(insrun, sizeof(insrun),
        "INSERT INTO tool_calls (session_id, entry_id, call_id, name, status)"
        " VALUES (%lld, 1, 'call_running', 'launch_agent', 'running');",
        (long long)tc_sid);
    assert(sqlite3_exec(db, insrun, NULL, NULL, NULL) == SQLITE_OK);

    /* Recover. */
    assert(db_recover_stale_sessions(db) == 0);

    /* Every stuck session is now idle with turn_iteration zeroed. */
    for (int i = 0; i < n_stuck; i++) {
        char *st = db_scalar_text(db, "SELECT state FROM sessions WHERE id=?;", ids[i]);
        assert(st && strcmp(st, "idle") == 0);
        free(st);
        assert((int)db_scalar_i64(db, "SELECT turn_iteration FROM sessions WHERE id=?", ids[i], -1) == 0);
    }

    /* The orphaned call no longer resurfaces as pending... */
    int n = -1;
    PendingToolCall *p = db_tool_call_get_pending(db, tc_sid, &n);
    assert(n == 0);
    db_tool_call_free_pending(p, n);

    /* ...and a synthetic error tool_result keeps the branch well-formed. */
    int64_t res = db_scalar_i64(db,
        "SELECT COUNT(*) FROM entries WHERE session_id=? AND type='tool_result'"
        " AND tool_call_id='call_orphan';", tc_sid, -1);
    assert(res == 1);

    /* The in-flight 'running' call is reconciled the same way: no longer
     * running, and answered by a synthetic tool_result. */
    int64_t still_running = db_scalar_i64(db,
        "SELECT COUNT(*) FROM tool_calls WHERE call_id='call_running'"
        " AND status='running';", -1, -1);
    assert(still_running == 0);
    int64_t res_run = db_scalar_i64(db,
        "SELECT COUNT(*) FROM entries WHERE session_id=? AND type='tool_result'"
        " AND tool_call_id='call_running';", tc_sid, -1);
    assert(res_run == 1);

    db_close(db);
}

static char *get_owner(sqlite3 *db, int64_t sid) {
    return db_scalar_text(db, "SELECT owner_instance FROM sessions WHERE id=?;", sid);
}

static void test_owner_instance_stamping(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "t", NULL, -1, 0);
    assert(sid > 0);

    /* With no instance set, busy state stamps NULL */
    assert(session_set_state(db, sid, "llm_running") == 0);
    char *owner = get_owner(db, sid);
    assert(owner == NULL);
    assert(session_set_state(db, sid, "idle") == 0);
    owner = get_owner(db, sid);
    assert(owner == NULL);

    /* Set an instance id, busy state stamps it */
    db_set_instance_id("test-inst-001");
    assert(session_set_state(db, sid, "llm_running") == 0);
    owner = get_owner(db, sid);
    assert(owner && strcmp(owner, "test-inst-001") == 0);
    free(owner);

    /* idle clears owner_instance */
    assert(session_set_state(db, sid, "idle") == 0);
    owner = get_owner(db, sid);
    assert(owner == NULL);

    /* Reset global for other tests */
    db_set_instance_id(NULL);
    db_close(db);
}

static void test_recovery_owner_scoped(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);

    /* Register a live process */
    char live_id[64];
    assert(process_register(db, "cli", getpid(), live_id, sizeof(live_id)) == 0);

    /* Session 1: transient, owned by the live process — should NOT be reset */
    int64_t s1 = session_create(db, "t", NULL, -1, 0);
    assert(s1 > 0);
    force_state(db, s1, "llm_running", 3);
    char sql[256];
    snprintf(sql, sizeof(sql),
        "UPDATE sessions SET owner_instance='%s' WHERE id=%lld;",
        live_id, (long long)s1);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);

    /* Session 2: transient, owned by a dead (unregistered) instance */
    int64_t s2 = session_create(db, "t", NULL, -1, 0);
    assert(s2 > 0);
    force_state(db, s2, "tool_running", 5);
    snprintf(sql, sizeof(sql),
        "UPDATE sessions SET owner_instance='dead-instance-xyz' WHERE id=%lld;",
        (long long)s2);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);

    /* Session 3: transient, NULL owner — should be reset */
    int64_t s3 = session_create(db, "t", NULL, -1, 0);
    assert(s3 > 0);
    force_state(db, s3, "rate_limited", 2);

    /* Recover */
    assert(db_recover_stale_sessions(db) == 0);

    /* s1 untouched (live owner) */
    char *st = db_scalar_text(db, "SELECT state FROM sessions WHERE id=?;", s1);
    assert(st && strcmp(st, "llm_running") == 0);
    free(st);

    /* s2 reset (dead owner) */
    st = db_scalar_text(db, "SELECT state FROM sessions WHERE id=?;", s2);
    assert(st && strcmp(st, "idle") == 0);
    free(st);

    /* s3 reset (NULL owner) */
    st = db_scalar_text(db, "SELECT state FROM sessions WHERE id=?;", s3);
    assert(st && strcmp(st, "idle") == 0);
    free(st);

    process_unregister(db, live_id);
    db_close(db);
}

int main(void) {
    TEST_INIT();
    printf("test_session_state:\n");
    RUN(test_transitions);
    RUN(test_concurrent_acquire);
    RUN(test_turn_iteration);
    RUN(test_startup_recovery);
    RUN(test_owner_instance_stamping);
    RUN(test_recovery_owner_scoped);
    return 0;
}
