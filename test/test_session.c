#include "db.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define TEST_DB "/tmp/test_cclaw_session.sqlite"

static sqlite3 *setup(void) {
    unlink(TEST_DB);
    return db_open(TEST_DB);
}

static void teardown(sqlite3 *db) {
    db_close(db);
    unlink(TEST_DB);
}

static void test_session_create(void) {
    sqlite3 *db = setup();
    int64_t id = session_create(db, "test session", NULL, -1, 0);
    assert(id > 0);

    int64_t id2 = session_create(db, "another", NULL, -1, 0);
    assert(id2 > id);

    teardown(db);
    printf("  PASS test_session_create\n");
}

static void test_session_list(void) {
    sqlite3 *db = setup();
    session_create(db, "alpha", NULL, -1, 0);
    session_create(db, "beta", NULL, -1, 0);

    int count = 0;
    Session *list = session_list(db, &count);
    assert(count == 2);
    assert(list != NULL);
    assert(strcmp(list[0].name, "alpha") == 0);
    assert(strcmp(list[1].name, "beta") == 0);

    session_list_free(list, count);
    teardown(db);
    printf("  PASS test_session_list\n");
}

static void test_session_list_empty(void) {
    sqlite3 *db = setup();
    int count = 0;
    Session *list = session_list(db, &count);
    assert(count == 0);
    assert(list == NULL);

    teardown(db);
    printf("  PASS test_session_list_empty\n");
}

static void test_session_set_leaf(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "leaftest", NULL, -1, 0);

    int rc = session_set_leaf(db, sid, 42);
    assert(rc == 0);

    /* Verify via list */
    int count = 0;
    Session *list = session_list(db, &count);
    assert(count == 1);
    assert(list[0].leaf_id == 42);

    session_list_free(list, count);
    teardown(db);
    printf("  PASS test_session_set_leaf\n");
}

static void test_session_set_leaf_invalid(void) {
    sqlite3 *db = setup();
    /* Non-existent session */
    int rc = session_set_leaf(db, 9999, 1);
    assert(rc == -1);

    teardown(db);
    printf("  PASS test_session_set_leaf_invalid\n");
}

/* Helper: insert entry directly for branch testing — uses split columns */
static int64_t insert_entry(sqlite3 *db, int64_t session_id, int64_t parent_id, int role, const char *content) {
    const char *sql = "INSERT INTO entries (parent_id, session_id, role, content) VALUES (?,?,?,?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, parent_id);
    sqlite3_bind_int64(stmt, 2, session_id);
    sqlite3_bind_int(stmt, 3, role);
    sqlite3_bind_text(stmt, 4, content, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return -1;
    }
    int64_t id = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(stmt);
    return id;
}

static void test_session_get_branch(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "branch", NULL, -1, 0);

    /* Build chain: e1 → e2 → e3 (V14: parent_id linking) */
    int64_t e1 = insert_entry(db, sid, -1, ROLE_USER, "hello");
    int64_t e2 = insert_entry(db, sid, e1, ROLE_ASSISTANT, "hi");
    int64_t e3 = insert_entry(db, sid, e2, ROLE_USER, "bye");

    session_set_leaf(db, sid, e3);

    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    assert(count == 3);
    assert(branch != NULL);

    /* Root→leaf order */
    assert(branch[0].id == e1);
    assert(branch[0].parent_id == -1);
    assert(strcmp(branch[0].message.content, "hello") == 0);

    assert(branch[1].id == e2);
    assert(branch[1].parent_id == e1);

    assert(branch[2].id == e3);
    assert(branch[2].parent_id == e2);
    assert(strcmp(branch[2].message.content, "bye") == 0);

    entry_branch_free(branch, count);
    teardown(db);
    printf("  PASS test_session_get_branch\n");
}

static void test_session_get_branch_empty(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "empty", NULL, -1, 0);
    /* leaf_id is -1 by default → no branch */
    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    assert(count == 0);
    assert(branch == NULL);

    teardown(db);
    printf("  PASS test_session_get_branch_empty\n");
}

/* T7: entry_append — linear chain via current leaf */
static void test_entry_append(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "append", NULL, -1, 0);

    Message m1 = { .role = ROLE_USER, .content = "hello" };
    int64_t e1 = entry_append(db, sid, &m1);
    assert(e1 > 0);

    Message m2 = { .role = ROLE_ASSISTANT, .content = "hi" };
    int64_t e2 = entry_append(db, sid, &m2);
    assert(e2 > e1);

    Message m3 = { .role = ROLE_USER, .content = "bye" };
    int64_t e3 = entry_append(db, sid, &m3);
    assert(e3 > e2);

    /* Verify branch is correct chain */
    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    assert(count == 3);
    assert(branch[0].id == e1);
    assert(branch[0].parent_id == -1);
    assert(branch[1].id == e2);
    assert(branch[1].parent_id == e1);
    assert(branch[2].id == e3);
    assert(branch[2].parent_id == e2);

    entry_branch_free(branch, count);
    teardown(db);
    printf("  PASS test_entry_append\n");
}

/* T7/V14: branching — entry_append_at forks from non-leaf */
static void test_entry_append_at_branch(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "branching", NULL, -1, 0);

    Message m1 = { .role = ROLE_USER, .content = "root" };
    int64_t e1 = entry_append(db, sid, &m1);

    Message m2 = { .role = ROLE_ASSISTANT, .content = "branch-a" };
    int64_t e2 = entry_append(db, sid, &m2);
    (void)e2;

    /* Fork from e1 (not current leaf e2) */
    Message m3 = { .role = ROLE_ASSISTANT, .content = "branch-b" };
    int64_t e3 = entry_append_at(db, sid, e1, &m3);
    assert(e3 > 0);

    /* Leaf should now be e3 */
    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    assert(count == 2);
    assert(branch[0].id == e1);
    assert(branch[0].parent_id == -1);
    assert(branch[1].id == e3);
    assert(branch[1].parent_id == e1);
    assert(strcmp(branch[1].message.content, "branch-b") == 0);

    entry_branch_free(branch, count);
    teardown(db);
    printf("  PASS test_entry_append_at_branch\n");
}

/* T7: entry_append on invalid session */
static void test_entry_append_invalid_session(void) {
    sqlite3 *db = setup();
    Message m = { .role = ROLE_USER, .content = "nope" };
    int64_t id = entry_append(db, 9999, &m);
    assert(id == -1);

    teardown(db);
    printf("  PASS test_entry_append_invalid_session\n");
}

/* T16: round-trip tool_calls and tool_result through DB */
static void test_entry_tool_calls_roundtrip(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "tools", NULL, -1, 0);

    /* Append user message */
    Message m1 = { .role = ROLE_USER, .content = "do something" };
    int64_t e1 = entry_append(db, sid, &m1);
    assert(e1 > 0);

    /* Append assistant message with tool_calls */
    ToolCall tc = { .id = "call_123", .name = "shell_exec", .arguments = "{\"cmd\":\"ls\"}" };
    Message m2 = { .role = ROLE_ASSISTANT, .content = NULL,
                   .tool_calls = &tc, .tool_call_count = 1 };
    int64_t e2 = entry_append(db, sid, &m2);
    assert(e2 > 0);

    /* Append tool result */
    ToolResult tr = { .tool_call_id = "call_123", .content = "file1.txt\nfile2.txt" };
    Message m3 = { .role = ROLE_TOOL, .tool_result = &tr };
    int64_t e3 = entry_append(db, sid, &m3);
    assert(e3 > 0);

    /* Read back branch and verify deserialization */
    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    assert(count == 3);

    /* Entry 0: user */
    assert(branch[0].message.role == ROLE_USER);
    assert(strcmp(branch[0].message.content, "do something") == 0);
    assert(branch[0].message.tool_calls == NULL);
    assert(branch[0].message.tool_result == NULL);

    /* Entry 1: assistant with tool_calls */
    assert(branch[1].message.role == ROLE_ASSISTANT);
    assert(branch[1].message.content == NULL);
    assert(branch[1].message.tool_call_count == 1);
    assert(branch[1].message.tool_calls != NULL);
    assert(strcmp(branch[1].message.tool_calls[0].id, "call_123") == 0);
    assert(strcmp(branch[1].message.tool_calls[0].name, "shell_exec") == 0);
    assert(strcmp(branch[1].message.tool_calls[0].arguments, "{\"cmd\":\"ls\"}") == 0);

    /* Entry 2: tool result */
    assert(branch[2].message.role == ROLE_TOOL);
    assert(branch[2].message.tool_result != NULL);
    assert(strcmp(branch[2].message.tool_result->tool_call_id, "call_123") == 0);
    assert(strcmp(branch[2].message.tool_result->content, "file1.txt\nfile2.txt") == 0);

    entry_branch_free(branch, count);
    teardown(db);
    printf("  PASS test_entry_tool_calls_roundtrip\n");
}

/* T159/V59: original_parent_id — NULL in DB maps to -1, set on reparent */
static void test_original_parent_id(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "reparent", NULL, -1, 0);

    int64_t e1 = insert_entry(db, sid, -1, ROLE_USER, "first");
    int64_t e2 = insert_entry(db, sid, e1, ROLE_ASSISTANT, "second");
    session_set_leaf(db, sid, e2);

    /* Default: original_parent_id is NULL → -1 in struct */
    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    assert(count == 2);
    assert(branch[0].original_parent_id == -1);
    assert(branch[1].original_parent_id == -1);
    entry_branch_free(branch, count);

    /* Simulate reparent: set original_parent_id on e2 */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "UPDATE entries SET original_parent_id=? WHERE id=?", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, e1);
    sqlite3_bind_int64(stmt, 2, e2);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    branch = session_get_branch(db, sid, &count);
    assert(count == 2);
    assert(branch[0].original_parent_id == -1); /* never reparented */
    assert(branch[1].original_parent_id == e1); /* reparented */
    entry_branch_free(branch, count);

    teardown(db);
    printf("  PASS test_original_parent_id\n");
}

int main(void) {
    printf("test_session:\n");
    test_session_create();
    test_session_list();
    test_session_list_empty();
    test_session_set_leaf();
    test_session_set_leaf_invalid();
    test_session_get_branch();
    test_session_get_branch_empty();
    test_entry_append();
    test_entry_append_at_branch();
    test_entry_append_invalid_session();
    test_entry_tool_calls_roundtrip();
    test_original_parent_id();
    printf("All session tests passed.\n");
    return 0;
}
