#include "db.h"
#include "test_util.h"
#include <assert.h>

static int64_t insert_entry(sqlite3 *db, int64_t session_id, int64_t parent_id,
                            int role, const char *content) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "INSERT INTO entries (parent_id, session_id, role, content)"
                               " VALUES (?,?,?,?);", -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, parent_id);
    sqlite3_bind_int64(stmt, 2, session_id);
    sqlite3_bind_int(stmt, 3, role);
    sqlite3_bind_text(stmt, 4, content, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? sqlite3_last_insert_rowid(db) : -1;
}

static void exec_ids(sqlite3 *db, const char *sql, int64_t a, int64_t b) {
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_int64(st, 1, a);
    sqlite3_bind_int64(st, 2, b);
    assert(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
}

/* The branch is the parent chain from the session's leaf, root first, and
 * carries original_parent_id (NULL → -1) for reparented entries. */
static void test_session_get_branch(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "branch", NULL, -1, 0);
    int count = 0;
    assert(session_get_branch(db, sid, &count) == NULL && count == 0);

    int64_t e1 = insert_entry(db, sid, -1, ROLE_USER, "hello");
    int64_t e2 = insert_entry(db, sid, e1, ROLE_ASSISTANT, "hi");
    int64_t e3 = insert_entry(db, sid, e2, ROLE_USER, "bye");
    exec_ids(db, "UPDATE sessions SET leaf_id=? WHERE id=?", e3, sid);
    exec_ids(db, "UPDATE entries SET original_parent_id=? WHERE id=?", e1, e2);

    Entry *b = session_get_branch(db, sid, &count);
    assert(count == 3);
    assert(b[0].id == e1 && b[0].parent_id == -1);
    assert(b[1].id == e2 && b[1].parent_id == e1);
    assert(b[2].id == e3 && b[2].parent_id == e2);
    assert(strcmp(b[2].message.content, "bye") == 0);
    assert(b[0].original_parent_id == -1);
    assert(b[1].original_parent_id == e1);
    entry_branch_free(b, count);
    db_close(db);
}

/* session_tool_allowed is an authority line: NULL filter = unrestricted,
 * an explicit list restricts, an unknown session fails closed, and a filter
 * that is not a JSON array is refused at create time. */
static void test_tool_filter(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t open = session_create(db, "open", NULL, -1, 0);
    assert(session_tool_allowed(db, open, "file_read") == 1);
    assert(session_tool_allowed(db, open, "anything_goes") == 1);

    int64_t sid = session_create_filtered(db, "scoped", NULL, -1, 0,
                                          "[\"file_read\",\"shell_exec\"]");
    assert(sid > 0);
    assert(session_tool_allowed(db, sid, "file_read") == 1);
    assert(session_tool_allowed(db, sid, "shell_exec") == 1);
    assert(session_tool_allowed(db, sid, "memory_block_create") == 0);

    assert(session_tool_allowed(db, 99999, "file_read") == 0);

    static const char *bad[] = { "not-json", "{\"x\":1}", "\"hello\"" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (session_create_filtered(db, "bad", NULL, -1, 0, bad[i]) != -1) {
            fprintf(stderr, "filter %s accepted\n", bad[i]);
            assert(0);
        }
    }
    db_close(db);
}

int main(void) {
    TEST_INIT();
    RUN(test_session_get_branch);
    RUN(test_tool_filter);
    return 0;
}
