#include "db.h"
#include "test_util.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define TEST_DB "/tmp/test_cclaw_session.sqlite"

static sqlite3 *setup(void) {
    test_db_clean(TEST_DB);
    return test_db_open(TEST_DB);
}

static void teardown(sqlite3 *db) {
    db_close(db);
    test_db_clean(TEST_DB);
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

/* Helper: set session leaf directly (append paths do this via trigger) */
static void set_leaf(sqlite3 *db, int64_t sid, int64_t leaf_id) {
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db, "UPDATE sessions SET leaf_id=? WHERE id=?", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_int64(st, 1, leaf_id);
    sqlite3_bind_int64(st, 2, sid);
    assert(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
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

    /* Build chain: e1 → e2 → e3 (parent_id linking) */
    int64_t e1 = insert_entry(db, sid, -1, ROLE_USER, "hello");
    int64_t e2 = insert_entry(db, sid, e1, ROLE_ASSISTANT, "hi");
    int64_t e3 = insert_entry(db, sid, e2, ROLE_USER, "bye");

    set_leaf(db, sid, e3);

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

/* entry_append — linear chain via current leaf */
static void test_entry_append(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "append", NULL, -1, 0);

    Message m1 = { .role = ROLE_USER, .content = "hello" };
    int64_t e1 = entry_append_with_iteration(db, sid, &m1, 1);
    assert(e1 > 0);

    Message m2 = { .role = ROLE_ASSISTANT, .content = "hi" };
    int64_t e2 = entry_append_with_iteration(db, sid, &m2, 1);
    assert(e2 > e1);

    Message m3 = { .role = ROLE_USER, .content = "bye" };
    int64_t e3 = entry_append_with_iteration(db, sid, &m3, 1);
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


/* entry_append on invalid session */
static void test_entry_append_invalid_session(void) {
    sqlite3 *db = setup();
    Message m = { .role = ROLE_USER, .content = "nope" };
    int64_t id = entry_append_with_iteration(db, 9999, &m, 1);
    assert(id == -1);

    teardown(db);
    printf("  PASS test_entry_append_invalid_session\n");
}

/* round-trip tool_calls and tool_result through DB */
static void test_entry_tool_calls_roundtrip(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "tools", NULL, -1, 0);

    /* Append user message */
    Message m1 = { .role = ROLE_USER, .content = "do something" };
    int64_t e1 = entry_append_with_iteration(db, sid, &m1, 1);
    assert(e1 > 0);

    /* Append assistant message with tool_calls */
    ToolCall tc = { .id = "call_123", .name = "shell_exec", .arguments = "{\"cmd\":\"ls\"}" };
    Message m2 = { .role = ROLE_ASSISTANT, .content = NULL,
                   .tool_calls = &tc, .tool_call_count = 1 };
    int64_t e2 = entry_append_with_iteration(db, sid, &m2, 1);
    assert(e2 > 0);

    /* Append tool result */
    ToolResult tr = { .tool_call_id = "call_123", .content = "file1.txt\nfile2.txt" };
    Message m3 = { .role = ROLE_TOOL, .tool_result = &tr };
    int64_t e3 = entry_append_with_iteration(db, sid, &m3, 1);
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

/* original_parent_id — NULL in DB maps to -1, set on reparent */
static void test_original_parent_id(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "reparent", NULL, -1, 0);

    int64_t e1 = insert_entry(db, sid, -1, ROLE_USER, "first");
    int64_t e2 = insert_entry(db, sid, e1, ROLE_ASSISTANT, "second");
    set_leaf(db, sid, e2);

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

/* session picker query — verify first/last prompt retrieval */
static void test_session_picker_query(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "picker", NULL, -1, 0);

    /* Insert user messages (role=1) */
    insert_entry(db, sid, -1, ROLE_USER, "hello world first message");
    int64_t e2 = insert_entry(db, sid, 1, ROLE_ASSISTANT, "hi there");
    int64_t e3 = insert_entry(db, sid, e2, ROLE_USER, "goodbye last message");
    (void)e3;

    /* Run the same query used by cli_select_session */
    const char *sql =
        "SELECT s.id, s.created_at,"
        " (SELECT substr(e.content,1,50) FROM entries e WHERE e.session_id=s.id AND e.role=1 ORDER BY e.id ASC LIMIT 1),"
        " (SELECT substr(e.content,1,50) FROM entries e WHERE e.session_id=s.id AND e.role=1 ORDER BY e.id DESC LIMIT 1)"
        " FROM sessions s ORDER BY s.updated_at DESC;";
    sqlite3_stmt *stmt;
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);

    int64_t qid = sqlite3_column_int64(stmt, 0);
    assert(qid == sid);
    assert(sqlite3_column_int64(stmt, 1) > 0); /* created_at */
    const char *first = (const char *)sqlite3_column_text(stmt, 2);
    const char *last = (const char *)sqlite3_column_text(stmt, 3);
    assert(first != NULL);
    assert(last != NULL);
    assert(strcmp(first, "hello world first message") == 0);
    assert(strcmp(last, "goodbye last message") == 0);

    sqlite3_finalize(stmt);

    /* Verify empty session returns NULLs for prompts */
    int64_t sid2 = session_create(db, "empty_picker", NULL, -1, 0);
    (void)sid2;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    /* Skip first row (sid), get second (sid2 — ordered by updated_at DESC so sid2 first) */
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    /* sid2 was just created so has latest updated_at */
    int64_t qid2 = sqlite3_column_int64(stmt, 0);
    if (qid2 == sid2) {
        assert(sqlite3_column_type(stmt, 2) == SQLITE_NULL);
        assert(sqlite3_column_type(stmt, 3) == SQLITE_NULL);
    }
    sqlite3_finalize(stmt);

    teardown(db);
    printf("  PASS test_session_picker_query\n");
}

/* session_tool_allowed tests */
static void test_tool_allowed_null_filter(void) {
    sqlite3 *db = setup();
    /* session_create uses NULL filter by default → unrestricted */
    int64_t sid = session_create(db, "open", NULL, -1, 0);
    assert(sid > 0);
    assert(session_tool_allowed(db, sid, "file_read") == 1);
    assert(session_tool_allowed(db, sid, "shell_exec") == 1);
    assert(session_tool_allowed(db, sid, "anything_goes") == 1);
    teardown(db);
    printf("  PASS test_tool_allowed_null_filter\n");
}

static void test_tool_allowed_explicit_filter(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create_filtered(db, "scoped", NULL, -1, 0,
                                          "[\"file_read\",\"shell_exec\"]");
    assert(sid > 0);
    assert(session_tool_allowed(db, sid, "file_read") == 1);
    assert(session_tool_allowed(db, sid, "shell_exec") == 1);
    assert(session_tool_allowed(db, sid, "memory_create") == 0);
    teardown(db);
    printf("  PASS test_tool_allowed_explicit_filter\n");
}

static void test_tool_allowed_unknown_session(void) {
    sqlite3 *db = setup();
    /* Non-existent session → fail closed (0) */
    assert(session_tool_allowed(db, 99999, "file_read") == 0);
    teardown(db);
    printf("  PASS test_tool_allowed_unknown_session\n");
}

static void test_create_filtered_invalid_filter(void) {
    sqlite3 *db = setup();
    /* Non-array JSON must be rejected */
    int64_t sid = session_create_filtered(db, "bad", NULL, -1, 0, "not-json");
    assert(sid == -1);

    /* JSON object (not array) must also be rejected */
    sid = session_create_filtered(db, "bad2", NULL, -1, 0, "{\"x\":1}");
    assert(sid == -1);

    /* Plain string is also invalid */
    sid = session_create_filtered(db, "bad3", NULL, -1, 0, "\"hello\"");
    assert(sid == -1);

    teardown(db);
    printf("  PASS test_create_filtered_invalid_filter\n");
}

int main(void) {
    TEST_INIT();
    printf("test_session:\n");
    test_session_create();
    test_session_get_branch();
    test_session_get_branch_empty();
    test_entry_append();
    test_entry_append_invalid_session();
    test_entry_tool_calls_roundtrip();
    test_original_parent_id();
    test_session_picker_query();
    test_tool_allowed_null_filter();
    test_tool_allowed_explicit_filter();
    test_tool_allowed_unknown_session();
    test_create_filtered_invalid_filter();
    printf("All session tests passed.\n");
    return 0;
}
