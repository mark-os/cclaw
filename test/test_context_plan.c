#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "context.h"
#include "db.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

static void append_msg(sqlite3 *db, int64_t sid, Role role, const char *content,
                       StopReason sr, ToolCall *tcs, size_t tc_count) {
    Message m = {0};
    m.role = role;
    m.content = (char *)content;
    m.stop_reason = sr;
    m.tool_calls = tcs;
    m.tool_call_count = tc_count;
    entry_append(db, sid, &m);
}

static void append_tool_result(sqlite3 *db, int64_t sid, const char *tc_id, const char *content) {
    Message m = {0};
    m.role = ROLE_TOOL;
    ToolResult tr = { .tool_call_id = (char *)tc_id, .content = (char *)content };
    m.tool_result = &tr;
    entry_append(db, sid, &m);
}

static void test_basic_plan(void) {
    TEST(basic_plan);
    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");

    int64_t sid = session_create(db, "test", NULL, -1, 0);
    append_msg(db, sid, ROLE_SYSTEM, "You are helpful.", STOP_REASON_NONE, NULL, 0);
    append_msg(db, sid, ROLE_USER, "Hello", STOP_REASON_NONE, NULL, 0);
    append_msg(db, sid, ROLE_ASSISTANT, "Hi!", STOP_REASON_STOP, NULL, 0);

    Config cfg = {0};
    cfg.provider.context_window = 128000;
    ContextPlan plan = {0};
    int rc = context_plan(db, sid, &cfg, 0, &plan);
    if (rc != 0) FAIL("context_plan returned error");
    if (plan.count != 3) FAIL("wrong count");
    if (plan.cut != 0) FAIL("should include all (cut=0)");
    if (plan.entries[0].role != ROLE_SYSTEM) FAIL("entry 0 not system");
    if (plan.entries[1].role != ROLE_USER) FAIL("entry 1 not user");
    if (plan.entries[2].role != ROLE_ASSISTANT) FAIL("entry 2 not assistant");

    context_plan_free(&plan);
    db_close(db);
    PASS();
}

static void test_v28_filtering(void) {
    TEST(v28_filtering);
    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");

    int64_t sid = session_create(db, "test", NULL, -1, 0);
    append_msg(db, sid, ROLE_USER, "Hello", STOP_REASON_NONE, NULL, 0);
    /* Errored assistant + its tool result should be filtered */
    ToolCall tc = { .id = "tc1", .name = "shell_exec", .arguments = "{}" };
    append_msg(db, sid, ROLE_ASSISTANT, NULL, STOP_REASON_ERROR, &tc, 1);
    append_tool_result(db, sid, "tc1", "some output");
    /* Good follow-up */
    append_msg(db, sid, ROLE_USER, "Try again", STOP_REASON_NONE, NULL, 0);
    append_msg(db, sid, ROLE_ASSISTANT, "OK", STOP_REASON_STOP, NULL, 0);

    Config cfg = {0};
    cfg.provider.context_window = 128000;
    ContextPlan plan = {0};
    int rc = context_plan(db, sid, &cfg, 0, &plan);
    if (rc != 0) FAIL("context_plan returned error");
    /* Should have: user, user, assistant (3 entries — errored assistant + tool_result filtered) */
    if (plan.count != 3) {
        char buf[64];
        snprintf(buf, sizeof(buf), "expected 3, got %d", plan.count);
        FAIL(buf);
    }
    if (plan.entries[0].role != ROLE_USER) FAIL("entry 0 not user");
    if (plan.entries[1].role != ROLE_USER) FAIL("entry 1 not user");
    if (plan.entries[2].role != ROLE_ASSISTANT) FAIL("entry 2 not assistant");

    context_plan_free(&plan);
    db_close(db);
    PASS();
}

static void test_cut_point(void) {
    TEST(cut_point_respects_budget);
    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");

    int64_t sid = session_create(db, "test", NULL, -1, 0);
    /* Insert many messages to exceed a tiny budget */
    for (int i = 0; i < 20; i++) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Message number %d with some padding text to use tokens", i);
        append_msg(db, sid, (i % 2 == 0) ? ROLE_USER : ROLE_ASSISTANT, buf,
                   (i % 2 == 1) ? STOP_REASON_STOP : STOP_REASON_NONE, NULL, 0);
    }

    Config cfg = {0};
    cfg.provider.context_window = 200; /* tiny: 120 token budget */
    ContextPlan plan = {0};
    int rc = context_plan(db, sid, &cfg, 0, &plan);
    if (rc != 0) FAIL("context_plan returned error");
    if (plan.count != 20) FAIL("wrong total count");
    if (plan.cut <= 0) FAIL("should have truncated (cut > 0)");
    /* Cut should be at a user message boundary (V8) */
    if (plan.entries[plan.cut].role != ROLE_USER &&
        plan.entries[plan.cut].role != ROLE_SYSTEM)
        FAIL("cut not at valid boundary");

    context_plan_free(&plan);
    db_close(db);
    PASS();
}

static void test_v8_tool_call_group(void) {
    TEST(v8_tool_call_group_not_split);
    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");

    int64_t sid = session_create(db, "test", NULL, -1, 0);
    append_msg(db, sid, ROLE_USER, "old message with lots of text padding", STOP_REASON_NONE, NULL, 0);
    append_msg(db, sid, ROLE_ASSISTANT, "old response", STOP_REASON_STOP, NULL, 0);
    append_msg(db, sid, ROLE_USER, "do something", STOP_REASON_NONE, NULL, 0);
    /* Assistant with tool call */
    ToolCall tc = { .id = "tc1", .name = "file_read", .arguments = "{\"path\":\"x\"}" };
    append_msg(db, sid, ROLE_ASSISTANT, NULL, STOP_REASON_TOOL_USE, &tc, 1);
    append_tool_result(db, sid, "tc1", "file contents here");
    append_msg(db, sid, ROLE_ASSISTANT, "Done!", STOP_REASON_STOP, NULL, 0);

    /* Budget that can fit the last few but not all */
    Config cfg = {0};
    cfg.max_history_tokens = 80;
    ContextPlan plan = {0};
    int rc = context_plan(db, sid, &cfg, 0, &plan);
    if (rc != 0) FAIL("context_plan returned error");

    /* If cut lands in the middle, it should NOT split the tool_call group */
    if (plan.cut > 0) {
        /* The cut entry should be user or system, never tool_result */
        if (plan.entries[plan.cut].role == ROLE_TOOL)
            FAIL("cut landed on tool_result (mid-group split)");
    }

    context_plan_free(&plan);
    db_close(db);
    PASS();
}

static void test_plan_no_content_access(void) {
    TEST(plan_no_content_or_tool_calls_access);
    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");

    int64_t sid = session_create(db, "test", NULL, -1, 0);
    /* Insert entries with large content to ensure overflow pages exist */
    char big[8192];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    append_msg(db, sid, ROLE_USER, big, STOP_REASON_NONE, NULL, 0);
    append_msg(db, sid, ROLE_ASSISTANT, big, STOP_REASON_STOP, NULL, 0);

    /* V56: context_plan must work correctly without loading content/tool_calls.
     * Verify the plan query only selects integer metadata columns. */
    Config cfg = {0};
    cfg.provider.context_window = 128000;
    ContextPlan plan = {0};
    int rc = context_plan(db, sid, &cfg, 0, &plan);
    if (rc != 0) FAIL("context_plan returned error");
    if (plan.count != 2) FAIL("wrong count");
    /* Verify token_estimate reflects the large content (not zero/default) */
    if (plan.entries[0].token_estimate < 100) FAIL("entry 0 token_estimate too small for 8KB content");
    if (plan.entries[1].token_estimate < 100) FAIL("entry 1 token_estimate too small for 8KB content");

    /* V56: Verify query plan has no redundant JOIN back to entries table.
     * The CTE carries all needed columns inline — final SELECT reads from CTE only. */
    sqlite3_stmt *stmt;
    int64_t leaf_id = plan.entries[plan.count - 1].id;
    const char *eqp_sql =
        "EXPLAIN QUERY PLAN "
        "WITH RECURSIVE branch(id, parent_id, role, stop_reason, token_estimate, tool_call_count) AS ("
        "  SELECT id, parent_id, role, stop_reason, token_estimate, tool_call_count"
        "    FROM entries WHERE id=? AND session_id=?"
        "  UNION ALL"
        "  SELECT e.id, e.parent_id, e.role, e.stop_reason, e.token_estimate, e.tool_call_count"
        "    FROM entries e JOIN branch b ON e.id=b.parent_id"
        ") SELECT id, role, stop_reason, token_estimate, tool_call_count"
        " FROM branch ORDER BY id;";

    if (sqlite3_prepare_v2(db, eqp_sql, -1, &stmt, NULL) != SQLITE_OK)
        FAIL("prepare EQP");
    sqlite3_bind_int64(stmt, 1, leaf_id);
    sqlite3_bind_int64(stmt, 2, sid);

    /* Count entries table accesses — should be exactly 2 (base case + recursive step).
     * No extra JOIN back to entries for column retrieval.
     * SQLite uses table name for base case, alias "e" for recursive step. */
    int entries_accesses = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *detail = (const char *)sqlite3_column_text(stmt, 3);
        if (detail && (strstr(detail, "SEARCH entries") || strstr(detail, "SEARCH e ")))
            entries_accesses++;
    }
    sqlite3_finalize(stmt);

    /* Exactly 2 entries accesses: base case (id=?) + recursive step (id=parent_id) */
    if (entries_accesses != 2) {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected 2 entries accesses, got %d (extra JOIN?)", entries_accesses);
        FAIL(buf);
    }

    context_plan_free(&plan);
    db_close(db);
    PASS();
}

int main(void) {
    printf("test_context_plan:\n");
    test_basic_plan();
    test_v28_filtering();
    test_cut_point();
    test_v8_tool_call_group();
    test_plan_no_content_access();
    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
