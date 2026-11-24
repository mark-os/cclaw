/* Reading a branch that holds a summary in the shape the deleted C
 * compaction left in existing databases (test_legacy_compact): the walk,
 * the remembered original parent, and full-text search over what it
 * replaced. */
#include "db.h"
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

    /* Compact: keep e1, summarize e2+e3, reparent e4 to summary */
    int64_t cid = test_legacy_compact(db, sid, e1, e4, "Summary of msg2 and msg3");
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
    /* The re-pointed entry remembers its old parent; the summary has none. */
    int64_t e3 = db_scalar_i64(db, "SELECT id FROM entries WHERE content='msg3'", 0, -1);
    assert(branch[2].original_parent_id == e3);
    assert(branch[1].original_parent_id == -1);

    entry_branch_free(branch, count);
    teardown(db);
}

/* old entries still reachable via FTS5 */
static void test_compaction_fts_indexes_old(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "compact_fts", NULL, -1, 0);

    Message m = {.role = ROLE_USER, .content = "first"};
    int64_t e1 = entry_append_with_iteration(db, sid, &m, 1);
    m.content = "unique_searchable_word";
    int64_t e2 = entry_append_with_iteration(db, sid, &m, 1);
    m.content = "final";
    int64_t e3 = entry_append_with_iteration(db, sid, &m, 1);

    /* Compact: e3 is reparented past e2, so e2 is the entry that leaves the
     * active branch. It must stay in FTS5 — compaction never deletes. */
    test_legacy_compact(db, sid, e1, e3, "Summary");

    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db,
        "SELECT e.id FROM entries_fts f JOIN entries e ON e.id = f.rowid"
        " WHERE entries_fts MATCH ?1 AND e.session_id = ?2 ORDER BY rank", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, "unique_searchable_word", -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, sid);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_int64(st, 0) == e2);

    /* Also verify compaction summary is searchable */
    sqlite3_reset(st);
    sqlite3_bind_text(st, 1, "Summary", -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, sid);
    assert(sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    teardown(db);
}

int main(void) {
    TEST_INIT();
    printf("test_compaction:\n");
    RUN(test_compaction_cte_stops);
    RUN(test_compaction_fts_indexes_old);
    return 0;
}
