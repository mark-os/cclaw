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
    int rc = context_plan(db, sid, &cfg, &plan);
    if (rc != 0) FAIL("context_plan failed");
    if (plan.count != 2) FAIL("wrong count");
    if (plan.entries[0].token_estimate <= 0) FAIL("entry 0 token_estimate <= 0");
    if (plan.entries[1].token_estimate <= 0) FAIL("entry 1 token_estimate <= 0");

    context_plan_free(&plan);
    db_close(db);
    PASS();
}

int main(void) {
    printf("test_entry_stats:\n");
    test_entry_stats_populated();
    test_context_plan_uses_stored_stats();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
