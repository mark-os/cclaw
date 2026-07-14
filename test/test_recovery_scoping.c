/* Test db_recover_stale_sessions owner scoping: live-owned sessions untouched,
 * dead-owned sessions reset with llm_jobs/tool_calls/approvals reconciled. */
#define _POSIX_C_SOURCE 200809L
#include "approval.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DB_PATH "/tmp/cclaw_test_recovery_scoping.db"

static void clean_db(void) {
    unlink(DB_PATH);
    unlink(DB_PATH "-wal");
    unlink(DB_PATH "-shm");
}

static sqlite3 *fresh_db(void) {
    clean_db();
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db != NULL);
    return db;
}

static void force_state(sqlite3 *db, int64_t sid, const char *state, const char *owner) {
    char sql[256];
    if (owner)
        snprintf(sql, sizeof(sql),
            "UPDATE sessions SET state='%s', owner_instance='%s' WHERE id=%lld;",
            state, owner, (long long)sid);
    else
        snprintf(sql, sizeof(sql),
            "UPDATE sessions SET state='%s', owner_instance=NULL WHERE id=%lld;",
            state, (long long)sid);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
}

static void test_recovery_scoping(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL);

    /* Instance A: register (live) */
    char id_a[64];
    assert(process_register(db, "cli", getpid(), id_a, sizeof(id_a)) == 0);

    /* Session owned by A, in transient state */
    db_set_instance_id(id_a);
    int64_t sid_a = session_create(db, "sa", "bot", -1, 0);
    assert(sid_a > 0);
    assert(session_set_state(db, sid_a, "llm_running") == 0);

    /* Instance B: just an id string, NOT registered (dead) */
    const char *id_b = "dead-instance-bbbbbbbbbbbbbbbb";

    /* Session owned by B, in transient state */
    int64_t sid_b = session_create(db, "sb", "bot", -1, 0);
    assert(sid_b > 0);
    force_state(db, sid_b, "tool_running", id_b);

    /* Insert an llm_jobs row for the B-owned session */
    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT INTO llm_jobs (session_id, agent_name) VALUES (%lld, 'bot');",
        (long long)sid_b);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);

    /* Insert a pending tool_call for sid_b (no tool_result) */
    snprintf(sql, sizeof(sql),
        "INSERT INTO tool_calls (session_id, entry_id, call_id, name, status)"
        " VALUES (%lld, 1, 'call_orphan_b', 'shell_exec', 'pending');",
        (long long)sid_b);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);

    /* Insert a pending approval for sid_b */
    int64_t appr_id = approval_create(db, sid_b, "call_orphan_b", "shell_exec",
                                      "request_changes",
                                      "{\"action\":\"request_changes\",\"changes\":{\"grants\":{\"hosts\":[\"x.com\"]}}}",
                                      "apply");
    assert(appr_id > 0);

    /* Session owned by B in a stale state but with NO orphaned tool_call —
     * recovered silently, no resume nudge (D6). */
    int64_t sid_c = session_create(db, "sc", "bot", -1, 0);
    assert(sid_c > 0);
    force_state(db, sid_c, "llm_running", id_b);

    /* Run recovery */
    assert(db_recover_stale_sessions(db) == 0);

    /* A-owned session: unchanged */
    char *st = db_scalar_text(db, "SELECT state FROM sessions WHERE id=?;", sid_a);
    assert(st && strcmp(st, "llm_running") == 0);
    free(st);
    char *own = db_scalar_text(db, "SELECT owner_instance FROM sessions WHERE id=?;", sid_a);
    assert(own && strcmp(own, id_a) == 0);
    free(own);

    /* B-owned session: reset to idle, owner NULL */
    st = db_scalar_text(db, "SELECT state FROM sessions WHERE id=?;", sid_b);
    assert(st && strcmp(st, "idle") == 0);
    free(st);
    own = db_scalar_text(db, "SELECT owner_instance FROM sessions WHERE id=?;", sid_b);
    assert(own == NULL);

    /* llm_jobs row deleted */
    int64_t jcnt = db_scalar_i64(db,
        "SELECT COUNT(*) FROM llm_jobs WHERE session_id=?;", sid_b, 0);
    assert(jcnt == 0);

    /* tool_call now status='done' */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db,
        "SELECT status FROM tool_calls WHERE call_id='call_orphan_b';", -1, &stmt, NULL);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "done") == 0);
    sqlite3_finalize(stmt);

    /* synthetic tool_result entry present */
    int64_t rcnt = db_scalar_i64(db,
        "SELECT COUNT(*) FROM entries WHERE session_id=? AND type='tool_result'"
        " AND tool_call_id='call_orphan_b';", sid_b, 0);
    assert(rcnt == 1);

    /* mid-work session got exactly one resume nudge in its inbox (review-5
     * F8 / D6): unconsumed, source 'system' */
    int64_t ncnt = db_scalar_i64(db,
        "SELECT COUNT(*) FROM inbox WHERE session_id=? AND consumed=0"
        " AND source='system';", sid_b, 0);
    assert(ncnt == 1);

    /* session recovered without reconciled tool_calls stays silent */
    st = db_scalar_text(db, "SELECT state FROM sessions WHERE id=?;", sid_c);
    assert(st && strcmp(st, "idle") == 0);
    free(st);
    ncnt = db_scalar_i64(db,
        "SELECT COUNT(*) FROM inbox WHERE session_id=?;", sid_c, 0);
    assert(ncnt == 0);

    /* approval state='denied' */
    sqlite3_prepare_v2(db,
        "SELECT state FROM approvals WHERE id=?;", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, appr_id);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "denied") == 0);
    sqlite3_finalize(stmt);

    process_unregister(db, id_a);
    db_set_instance_id(NULL);
    db_close(db);
    clean_db();
    printf("  PASS test_recovery_scoping\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_recovery_scoping:\n");
    test_recovery_scoping();
    printf("all recovery scoping tests passed\n");
    return 0;
}
