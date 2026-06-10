#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

/* Helper: query turn_id for an entry by id */
static int64_t get_entry_turn_id(sqlite3 *db, int64_t entry_id) {
    const char *sql = "SELECT turn_id FROM entries WHERE id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, entry_id);
    int64_t tid = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (sqlite3_column_type(stmt, 0) != SQLITE_NULL)
            tid = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return tid;
}

static void test_next_turn_id_empty_session(void) {
    TEST(next_turn_id_empty_session);
    sqlite3 *db = test_db_open(":memory:");
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    int64_t tid = db_next_turn_id(db, sid);
    if (tid != 1) { FAIL("expected 1 for empty session"); db_close(db); return; }
    db_close(db);
    PASS();
}

static void test_next_turn_id_increments(void) {
    TEST(next_turn_id_increments);
    sqlite3 *db = test_db_open(":memory:");
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message msg = {.role = ROLE_USER, .content = "hello"};
    entry_append_with_turn(db, sid, &msg, 1);

    int64_t tid = db_next_turn_id(db, sid);
    if (tid != 2) { FAIL("expected 2 after turn_id=1"); db_close(db); return; }

    entry_append_with_turn(db, sid, &msg, 2);
    tid = db_next_turn_id(db, sid);
    if (tid != 3) { FAIL("expected 3 after turn_id=2"); db_close(db); return; }

    db_close(db);
    PASS();
}

static void test_append_with_turn_tags_entry(void) {
    TEST(append_with_turn_tags_entry);
    sqlite3 *db = test_db_open(":memory:");
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message msg = {.role = ROLE_USER, .content = "hi"};
    int64_t eid = entry_append_with_turn(db, sid, &msg, 42);
    if (eid < 0) { FAIL("append failed"); db_close(db); return; }

    int64_t tid = get_entry_turn_id(db, eid);
    if (tid != 42) { FAIL("turn_id not set"); db_close(db); return; }

    db_close(db);
    PASS();
}

static void test_multiple_entries_same_turn(void) {
    TEST(multiple_entries_same_turn);
    sqlite3 *db = test_db_open(":memory:");
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    /* Simulate a turn: assistant + tool_result share turn_id=1 */
    Message asst = {.role = ROLE_ASSISTANT, .content = "calling tool"};
    int64_t e1 = entry_append_with_turn(db, sid, &asst, 1);

    ToolResult tr = {.tool_call_id = "tc1", .content = "result"};
    Message tool_msg = {.role = ROLE_TOOL, .tool_result = &tr};
    int64_t e2 = entry_append_with_turn(db, sid, &tool_msg, 1);

    if (get_entry_turn_id(db, e1) != 1) { FAIL("e1 turn_id wrong"); db_close(db); return; }
    if (get_entry_turn_id(db, e2) != 1) { FAIL("e2 turn_id wrong"); db_close(db); return; }

    /* Next turn should be 2 */
    int64_t next = db_next_turn_id(db, sid);
    if (next != 2) { FAIL("next turn_id should be 2"); db_close(db); return; }

    db_close(db);
    PASS();
}

static void test_entries_without_turn_id_ignored(void) {
    TEST(entries_without_turn_id_ignored);
    sqlite3 *db = test_db_open(":memory:");
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    /* entry_append (no turn_id) leaves turn_id NULL */
    Message msg = {.role = ROLE_USER, .content = "hi"};
    int64_t eid = entry_append(db, sid, &msg);
    int64_t tid = get_entry_turn_id(db, eid);
    if (tid != -1) { FAIL("expected NULL (-1) for untagged entry"); db_close(db); return; }

    /* next_turn_id still returns 1 since MAX(NULL) = NULL → COALESCE → 0+1=1 */
    int64_t next = db_next_turn_id(db, sid);
    if (next != 1) { FAIL("expected 1 when no turn_ids set"); db_close(db); return; }

    db_close(db);
    PASS();
}

int main(void) {
    printf("test_turn_tagging:\n");
    test_next_turn_id_empty_session();
    test_next_turn_id_increments();
    test_append_with_turn_tags_entry();
    test_multiple_entries_same_turn();
    test_entries_without_turn_id_ignored();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
