#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "db.h"
#include "context.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

static void test_entry_stats_populated(void) {
    TEST(entry_stats_populated);
    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");

    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message m = {0};
    m.role = ROLE_USER;
    m.content = "Hello world";
    int64_t eid = entry_append(db, sid, &m);
    if (eid < 0) FAIL("entry_append");

    /* Query stored stats */
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
        "SELECT token_estimate, content_bytes FROM entries WHERE id=?",
        -1, &stmt, NULL) != SQLITE_OK) FAIL("prepare");
    sqlite3_bind_int64(stmt, 1, eid);
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); FAIL("no row"); }

    int tok = sqlite3_column_int(stmt, 0);
    int bytes = sqlite3_column_int(stmt, 1);
    sqlite3_finalize(stmt);

    if (tok <= 0) FAIL("token_estimate should be > 0");
    if (bytes <= 0) FAIL("content_bytes should be > 0");
    /* token_estimate = bytes/4 + 4 */
    if (tok != (bytes / 4) + 4) FAIL("token_estimate != bytes/4 + 4");

    db_close(db);
    PASS();
}

static void test_context_plan_uses_stored_stats(void) {
    TEST(context_plan_uses_stored_stats);
    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");

    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message m = {0};
    m.role = ROLE_USER;
    m.content = "Hello";
    entry_append(db, sid, &m);

    Message m2 = {0};
    m2.role = ROLE_ASSISTANT;
    m2.content = "Hi there!";
    m2.stop_reason = STOP_REASON_STOP;
    entry_append(db, sid, &m2);

    Config cfg = {0};
    cfg.provider.context_window = 128000;
    ContextPlan plan = {0};
    int rc = context_plan(db, sid, &cfg, 0, &plan);
    if (rc != 0) FAIL("context_plan failed");
    if (plan.count != 2) FAIL("wrong count");
    if (plan.entries[0].token_estimate <= 0) FAIL("entry 0 token_estimate <= 0");
    if (plan.entries[1].token_estimate <= 0) FAIL("entry 1 token_estimate <= 0");

    context_plan_free(&plan);
    db_close(db);
    PASS();
}

static void test_split_columns_written(void) {
    TEST(split_columns_written);
    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");

    int64_t sid = session_create(db, "test", NULL, -1, 0);

    /* Assistant message with model + usage */
    Message asst = {0};
    asst.role = ROLE_ASSISTANT;
    asst.content = "Hello";
    asst.stop_reason = STOP_REASON_STOP;
    asst.model = "deepseek/deepseek-v4-flash";
    asst.usage_in = 100;
    asst.usage_out = 50;
    int64_t eid = entry_append(db, sid, &asst);
    if (eid < 0) FAIL("entry_append asst");

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
        "SELECT model, usage_in, usage_out FROM entries WHERE id=?",
        -1, &stmt, NULL) != SQLITE_OK) FAIL("prepare");
    sqlite3_bind_int64(stmt, 1, eid);
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); FAIL("no row"); }
    const char *model = (const char *)sqlite3_column_text(stmt, 0);
    if (!model || strcmp(model, "deepseek/deepseek-v4-flash") != 0) { sqlite3_finalize(stmt); FAIL("model mismatch"); }
    if (sqlite3_column_int(stmt, 1) != 100) { sqlite3_finalize(stmt); FAIL("usage_in"); }
    if (sqlite3_column_int(stmt, 2) != 50) { sqlite3_finalize(stmt); FAIL("usage_out"); }
    sqlite3_finalize(stmt);

    /* Tool result with tool_name + is_error */
    ToolResult tr = {.tool_call_id = "tc_1", .content = "error: not found"};
    Message tool_msg = {.role = ROLE_TOOL, .tool_result = &tr,
                        .tool_name = "file_read", .is_error = 1};
    int64_t tid = entry_append(db, sid, &tool_msg);
    if (tid < 0) FAIL("entry_append tool");

    if (sqlite3_prepare_v2(db,
        "SELECT tool_name, is_error, tool_call_id FROM entries WHERE id=?",
        -1, &stmt, NULL) != SQLITE_OK) FAIL("prepare2");
    sqlite3_bind_int64(stmt, 1, tid);
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); FAIL("no row2"); }
    const char *tn = (const char *)sqlite3_column_text(stmt, 0);
    if (!tn || strcmp(tn, "file_read") != 0) { sqlite3_finalize(stmt); FAIL("tool_name mismatch"); }
    if (sqlite3_column_int(stmt, 1) != 1) { sqlite3_finalize(stmt); FAIL("is_error should be 1"); }
    const char *tcid = (const char *)sqlite3_column_text(stmt, 2);
    if (!tcid || strcmp(tcid, "tc_1") != 0) { sqlite3_finalize(stmt); FAIL("tool_call_id mismatch"); }
    sqlite3_finalize(stmt);

    db_close(db);
    PASS();
}

int main(void) {
    printf("test_entry_stats:\n");
    test_entry_stats_populated();
    test_context_plan_uses_stored_stats();
    test_split_columns_written();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
