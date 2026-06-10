#include "db.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define TEST_DB "/tmp/test_cclaw_compaction.sqlite"

static sqlite3 *setup(void) {
    unlink(TEST_DB);
    return test_db_open(TEST_DB);
}

static void teardown(sqlite3 *db) {
    db_close(db);
    unlink(TEST_DB);
}

/* V58: CTE from leaf stops at compaction node */
static void test_compaction_cte_stops(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "compact_test", NULL, -1, 0);

    /* Build chain: e1 → e2 → e3 → e4 → e5 */
    Message m = {.role = ROLE_USER, .content = "msg1"};
    int64_t e1 = entry_append(db, sid, &m);
    m.content = "msg2";
    entry_append(db, sid, &m); /* e2 */
    m.content = "msg3";
    entry_append(db, sid, &m); /* e3 */
    m.content = "msg4";
    int64_t e4 = entry_append(db, sid, &m);
    m.content = "msg5";
    int64_t e5 = entry_append(db, sid, &m);
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

/* V59: original_parent_id populated on reparent */
static void test_compaction_original_parent(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "compact_orig", NULL, -1, 0);

    Message m = {.role = ROLE_USER, .content = "a"};
    int64_t e1 = entry_append(db, sid, &m);
    m.content = "b";
    int64_t e2 = entry_append(db, sid, &m);
    m.content = "c";
    int64_t e3 = entry_append(db, sid, &m);

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

/* V58: old entries reachable via forward walk from branch point */
static void test_compaction_forward_walk(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "compact_fwd", NULL, -1, 0);

    /* Build chain: e1 → e2 → e3 → e4 → e5 */
    Message m = {.role = ROLE_USER, .content = "first"};
    int64_t e1 = entry_append(db, sid, &m);
    m.content = "second";
    int64_t e2 = entry_append(db, sid, &m);
    m.content = "third";
    int64_t e3 = entry_append(db, sid, &m);
    m.content = "fourth";
    int64_t e4 = entry_append(db, sid, &m);
    m.content = "fifth";
    entry_append(db, sid, &m); /* e5 */

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

/* V58: old entries still reachable via FTS5 */
static void test_compaction_fts_indexes_old(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "compact_fts", NULL, -1, 0);

    Message m = {.role = ROLE_USER, .content = "unique_searchable_word"};
    int64_t e1 = entry_append(db, sid, &m);
    m.content = "another message";
    entry_append(db, sid, &m); /* e2 */
    m.content = "final";
    int64_t e3 = entry_append(db, sid, &m);

    /* Compact: reparent e3 past e1 (e1 becomes "old") */
    entry_compact(db, sid, e1, e3, "Summary");
    /* But we want to test that e1's content is still in FTS5 — it was never deleted */

    int count = 0;
    Entry *results = entry_search(db, "unique_searchable_word", sid, &count);
    assert(count >= 1);
    assert(results[0].id == e1);

    entry_branch_free(results, count);

    /* Also verify compaction summary is searchable */
    results = entry_search(db, "Summary", sid, &count);
    assert(count >= 1);

    entry_branch_free(results, count);
    teardown(db);
    printf("  PASS test_compaction_fts_indexes_old\n");
}

int main(void) {
    printf("test_compaction:\n");
    test_compaction_cte_stops();
    test_compaction_forward_walk();
    test_compaction_original_parent();
    test_compaction_fts_indexes_old();
    printf("All compaction tests passed.\n");
    return 0;
}
