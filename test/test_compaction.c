#include "db.h"
#include "llm_proc.h"
#include "test_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define TEST_DB "/tmp/test_cclaw_compaction.sqlite"

static sqlite3 *setup(void) {
    test_db_clean(TEST_DB);
    return test_db_open(TEST_DB);
}

static void teardown(sqlite3 *db) {
    db_close(db);
    test_db_clean(TEST_DB);
}

/* CTE from leaf stops at compaction node */
static void test_compaction_cte_stops(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "compact_test", NULL, -1, 0);

    /* Build chain: e1 → e2 → e3 → e4 → e5 */
    Message m = {.role = ROLE_USER, .content = "msg1"};
    int64_t e1 = entry_append_with_iteration(db, sid, &m, 1);
    m.content = "msg2";
    entry_append_with_iteration(db, sid, &m, 1); /* e2 */
    m.content = "msg3";
    entry_append_with_iteration(db, sid, &m, 1); /* e3 */
    m.content = "msg4";
    int64_t e4 = entry_append_with_iteration(db, sid, &m, 1);
    m.content = "msg5";
    int64_t e5 = entry_append_with_iteration(db, sid, &m, 1);
    (void)e5;

    /* Compact: keep e1, summarize e2+e3, reparent e4 to summary */
    int64_t cid = entry_compact(db, sid, e1, e4, "Summary of msg2 and msg3");
    assert(cid > 0);

    /* Branch from leaf should be: e1 → cid → e4 → e5 (skips e2, e3) */
    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    assert(branch != NULL);
    assert(count == 4);
    assert(branch[0].id == e1);
    assert(branch[1].id == cid);
    assert(branch[1].message.role == ROLE_COMPACTION);
    assert(strcmp(branch[1].message.content, "Summary of msg2 and msg3") == 0);
    assert(branch[2].id == e4);
    assert(branch[3].id == e5);

    entry_branch_free(branch, count);
    teardown(db);
    printf("  PASS test_compaction_cte_stops\n");
}

/* original_parent_id populated on reparent */
static void test_compaction_original_parent(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "compact_orig", NULL, -1, 0);

    Message m = {.role = ROLE_USER, .content = "a"};
    int64_t e1 = entry_append_with_iteration(db, sid, &m, 1);
    m.content = "b";
    int64_t e2 = entry_append_with_iteration(db, sid, &m, 1);
    m.content = "c";
    int64_t e3 = entry_append_with_iteration(db, sid, &m, 1);

    /* Compact: keep e1, reparent e3 to summary */
    int64_t cid = entry_compact(db, sid, e1, e3, "Summary of b");
    assert(cid > 0);

    /* Verify original_parent_id on e3 = e2 (its old parent) */
    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    assert(branch != NULL);
    /* branch: e1, cid, e3 */
    assert(count == 3);
    assert(branch[2].id == e3);
    assert(branch[2].original_parent_id == e2);
    /* compaction entry itself has no reparent */
    assert(branch[1].original_parent_id == -1);

    entry_branch_free(branch, count);
    teardown(db);
    printf("  PASS test_compaction_original_parent\n");
}

/* old entries reachable via forward walk from branch point */
static void test_compaction_forward_walk(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "compact_fwd", NULL, -1, 0);

    /* Build chain: e1 → e2 → e3 → e4 → e5 */
    Message m = {.role = ROLE_USER, .content = "first"};
    int64_t e1 = entry_append_with_iteration(db, sid, &m, 1);
    m.content = "second";
    int64_t e2 = entry_append_with_iteration(db, sid, &m, 1);
    m.content = "third";
    int64_t e3 = entry_append_with_iteration(db, sid, &m, 1);
    m.content = "fourth";
    int64_t e4 = entry_append_with_iteration(db, sid, &m, 1);
    m.content = "fifth";
    entry_append_with_iteration(db, sid, &m, 1); /* e5 */

    /* Compact: keep e1, summarize e2+e3, reparent e4 to summary */
    entry_compact(db, sid, e1, e4, "Summary of 2+3");

    /* Forward walk from e1: find all entries with parent_id = e1.
     * Should include both the compaction node AND the old e2. */
    const char *sql = "WITH RECURSIVE fwd(id, content, parent_id) AS ("
        "  SELECT id, content, parent_id FROM entries WHERE parent_id=? AND session_id=?"
        "  UNION ALL"
        "  SELECT e.id, e.content, e.parent_id FROM entries e JOIN fwd f ON e.parent_id=f.id"
        ") SELECT id, content FROM fwd ORDER BY id;";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    assert(rc == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, e1);
    sqlite3_bind_int64(stmt, 2, sid);

    int found_e2 = 0, found_e3 = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(stmt, 0);
        if (id == e2) found_e2 = 1;
        if (id == e3) found_e3 = 1;
    }
    sqlite3_finalize(stmt);

    /* Old entries e2, e3 still reachable via forward walk */
    assert(found_e2);
    assert(found_e3);

    teardown(db);
    printf("  PASS test_compaction_forward_walk\n");
}

/* old entries still reachable via FTS5 */
static void test_compaction_fts_indexes_old(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "compact_fts", NULL, -1, 0);

    Message m = {.role = ROLE_USER, .content = "unique_searchable_word"};
    int64_t e1 = entry_append_with_iteration(db, sid, &m, 1);
    m.content = "another message";
    entry_append_with_iteration(db, sid, &m, 1); /* e2 */
    m.content = "final";
    int64_t e3 = entry_append_with_iteration(db, sid, &m, 1);

    /* Compact: reparent e3 past e1 (e1 becomes "old") */
    entry_compact(db, sid, e1, e3, "Summary");
    /* But we want to test that e1's content is still in FTS5 — it was never deleted */

    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db,
        "SELECT e.id FROM entries_fts f JOIN entries e ON e.id = f.rowid"
        " WHERE entries_fts MATCH ?1 AND e.session_id = ?2 ORDER BY rank", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, "unique_searchable_word", -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, sid);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_int64(st, 0) == e1);

    /* Also verify compaction summary is searchable */
    sqlite3_reset(st);
    sqlite3_bind_text(st, 1, "Summary", -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, sid);
    assert(sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    teardown(db);
    printf("  PASS test_compaction_fts_indexes_old\n");
}

/* C4: the counter bumps on failure, resets on success, and the operator
 * notice fires exactly once per streak — never as a session entry. */
static void test_compaction_fail_notice(void) {
    sqlite3 *db = setup();
    test_seed_agent(db, "default");
    int64_t sid = session_create(db, "fail_test", "default", -1, 0);
    sqlite3_exec(db, "UPDATE sessions SET channel_name='opsch', chat_id='7';",
                 NULL, NULL, NULL);

    int64_t before = db_scalar_i64(db, "SELECT COUNT(*) FROM entries"
                                       " WHERE session_id=?;", sid, -1);

    compaction_record_outcome(db, NULL, sid, 0, 4000);
    compaction_record_outcome(db, NULL, sid, 0, 4000);
    assert(db_scalar_i64(db, "SELECT compaction_fail_count FROM sessions"
                             " WHERE id=?;", sid, -1) == 2);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM channel_outbox WHERE session_id=?;",
                         sid, -1) == 0);

    /* Transition to 3 → one notice; further failures stay quiet. */
    compaction_record_outcome(db, NULL, sid, 0, 4000);
    compaction_record_outcome(db, NULL, sid, 0, 4000);
    assert(db_scalar_i64(db, "SELECT compaction_fail_count FROM sessions"
                             " WHERE id=?;", sid, -1) == 4);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM channel_outbox WHERE session_id=?"
                             " AND payload LIKE '%compaction failing%'"
                             " AND payload LIKE '%4000 tokens%';", sid, -1) == 1);

    /* The agent is never told. */
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM entries WHERE session_id=?;",
                         sid, -1) == before);

    /* Success zeroes it; the next streak can notify again. */
    compaction_record_outcome(db, NULL, sid, 1, 4000);
    assert(db_scalar_i64(db, "SELECT compaction_fail_count FROM sessions"
                             " WHERE id=?;", sid, -1) == 0);
    for (int i = 0; i < 3; i++) compaction_record_outcome(db, NULL, sid, 0, 4000);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM channel_outbox WHERE session_id=?;",
                         sid, -1) == 2);

    teardown(db);
    printf("  PASS test_compaction_fail_notice\n");
}

/* E2: the mechanical coda names every open commitment, and says nothing at
 * all when there is none. */
static void test_state_coda(void) {
    sqlite3 *db = setup();
    test_seed_agent(db, "default");
    int64_t sid = session_create(db, "coda", "default", -1, 0);

    /* Empty state → no coda, not an empty marker. */
    assert(compaction_state_coda(db, sid) == NULL);

    /* A pending approval, an approved-but-unconsumed ticket, a decided one
     * (must NOT appear), a running child, an idle child (must not appear),
     * an armed one-shot and a recurring job (recurring must not appear). */
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO approvals (session_id, tool_name, state) VALUES"
        " (?1,'shell_exec','pending'), (?1,'web_fetch','approved'),"
        " (?1,'js_eval','consumed'), (?1,'file_write','rejected');",
        -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);

    int64_t kid = session_create(db, "child", "default", sid, 1);
    int64_t idle_kid = session_create(db, "idle_child", "default", sid, 1);
    assert(session_set_state(db, kid, "llm_running") == 0);
    (void)idle_kid;   /* stays 'idle' */

    assert(sqlite3_prepare_v2(db,
        "INSERT INTO cron_jobs (agent_name,name,cron_expr,run_at,session_id,task,"
        " next_run_at) VALUES"
        " ('default','ship-it','',unixepoch()+3600,?1,'post the release note',"
        "  unixepoch()+3600);",
        -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
    assert(sqlite3_exec(db,
        "INSERT INTO cron_jobs (agent_name,name,cron_expr,interval_s,session_id,task,"
        " next_run_at) VALUES ('default','hourly','0 * * * *',3600,1,'poll',"
        " unixepoch()+60);", NULL, NULL, NULL) == SQLITE_OK);

    char *coda = compaction_state_coda(db, sid);
    assert(coda);
    assert(strstr(coda, COMPACTION_CODA_MARKER));
    assert(strstr(coda, "shell_exec") && strstr(coda, "waiting on a human"));
    assert(!strstr(coda, "web_fetch"));  /* approved = decided = absent */
    assert(!strstr(coda, "js_eval") && !strstr(coda, "file_write"));
    assert(strstr(coda, "llm_running"));
    assert(!strstr(coda, "idle"));
    assert(strstr(coda, "ship-it") && strstr(coda, "post the release note"));
    assert(!strstr(coda, "hourly"));
    free(coda);

    teardown(db);
    printf("  PASS test_state_coda\n");
}

int main(void) {
    TEST_INIT();
    printf("test_compaction:\n");
    test_state_coda();
    test_compaction_fail_notice();
    test_compaction_cte_stops();
    test_compaction_forward_walk();
    test_compaction_original_parent();
    test_compaction_fts_indexes_old();
    printf("All compaction tests passed.\n");
    return 0;
}
