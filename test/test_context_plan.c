#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "context.h"
#include "db.h"
#include "test_util.h"

static void append_msg(sqlite3 *db, int64_t sid, Role role, const char *content,
                       StopReason sr, ToolCall *tcs, size_t tc_count) {
    Message m = {0};
    m.role = role;
    m.content = (char *)content;
    m.stop_reason = sr;
    m.tool_calls = tcs;
    m.tool_call_count = tc_count;
    entry_append_with_iteration(db, sid, &m, 1);
}

static void append_tool_result(sqlite3 *db, int64_t sid, const char *tc_id, const char *content) {
    Message m = {0};
    m.role = ROLE_TOOL;
    ToolResult tr = { .tool_call_id = (char *)tc_id, .content = (char *)content };
    m.tool_result = &tr;
    entry_append_with_iteration(db, sid, &m, 1);
}

static void test_basic_plan(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);

    int64_t sid = session_create(db, "test", NULL, -1, 0);
    append_msg(db, sid, ROLE_SYSTEM, "You are helpful.", STOP_REASON_NONE, NULL, 0);
    append_msg(db, sid, ROLE_USER, "Hello", STOP_REASON_NONE, NULL, 0);
    append_msg(db, sid, ROLE_ASSISTANT, "Hi!", STOP_REASON_STOP, NULL, 0);

    Config cfg = {0};
    cfg.context_window = 128000;
    ContextPlan plan = {0};
    int rc = context_plan(db, sid, &cfg, 0, &plan);
    assert(rc == 0);
    assert(plan.count == 3);
    assert(plan.cut == 0);
    assert(plan.entries[0].role == ROLE_SYSTEM);
    assert(plan.entries[1].role == ROLE_USER);
    assert(plan.entries[2].role == ROLE_ASSISTANT);

    context_plan_free(&plan);
    db_close(db);
}

static void test_v28_filtering(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);

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
    cfg.context_window = 128000;
    ContextPlan plan = {0};
    int rc = context_plan(db, sid, &cfg, 0, &plan);
    assert(rc == 0);
    /* Should have: user, user, assistant (3 entries — errored assistant + tool_result filtered) */
    assert(plan.count == 3);
    assert(plan.entries[0].role == ROLE_USER);
    assert(plan.entries[1].role == ROLE_USER);
    assert(plan.entries[2].role == ROLE_ASSISTANT);

    context_plan_free(&plan);
    db_close(db);
}

static void test_cut_point(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);

    int64_t sid = session_create(db, "test", NULL, -1, 0);
    /* Insert many messages to exceed a tiny budget */
    for (int i = 0; i < 20; i++) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Message number %d with some padding text to use tokens", i);
        append_msg(db, sid, (i % 2 == 0) ? ROLE_USER : ROLE_ASSISTANT, buf,
                   (i % 2 == 1) ? STOP_REASON_STOP : STOP_REASON_NONE, NULL, 0);
    }

    Config cfg = {0};
    cfg.context_window = 200; /* tiny: a 100-token budget (half the window) */
    ContextPlan plan = {0};
    int rc = context_plan(db, sid, &cfg, 0, &plan);
    assert(rc == 0);
    assert(plan.count == 20);
    assert(plan.cut > 0);
    /* Cut should be at a user message boundary */
    assert(!(plan.entries[plan.cut].role != ROLE_USER &&
        plan.entries[plan.cut].role != ROLE_SYSTEM));

    context_plan_free(&plan);
    db_close(db);
}

/* A turn legitimately opens with several consecutive user entries — the inbox
 * drain consumes a channel message, a job result and a sub-agent result in one
 * transaction (advance.c idle branch). Those entries are one turn and the cut
 * must never land between them. */
static void test_multi_user_turn_not_split(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);

    char pad[401];
    memset(pad, 'x', sizeof(pad) - 1);
    pad[sizeof(pad) - 1] = '\0';

    int64_t sid = session_create(db, "test", NULL, -1, 0);
    /* Turn 1 — plain question/answer. */
    append_msg(db, sid, ROLE_USER, pad, STOP_REASON_NONE, NULL, 0);
    append_msg(db, sid, ROLE_ASSISTANT, pad, STOP_REASON_STOP, NULL, 0);
    /* Turn 2 — opens with three inbox-drained user entries. */
    inbox_insert(db, sid, "channel", NULL, pad);
    inbox_insert(db, sid, "job_result", NULL, pad);
    inbox_insert(db, sid, "agent_result", NULL, pad);
    assert(inbox_consume_into_entries(db, sid, 100) == 3);
    append_msg(db, sid, ROLE_ASSISTANT, pad, STOP_REASON_STOP, NULL, 0);

    /* Budget floors at 256 tokens: the tail assistant + the last user entry fit,
     * the one before it does not — so the raw cut lands inside the drain. */
    Config cfg = {0};
    cfg.max_history_tokens = 1;
    ContextPlan plan = {0};
    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);
    assert(plan.count == 6);
    assert(plan.cut > 0);
    assert(!(plan.entries[plan.cut].role == ROLE_USER &&
        plan.entries[plan.cut - 1].role == ROLE_USER));
    assert(!(plan.entries[plan.cut].turn_id == plan.entries[plan.cut - 1].turn_id));

    context_plan_free(&plan);
    db_close(db);
}

static void test_plan_no_content_access(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);

    int64_t sid = session_create(db, "test", NULL, -1, 0);
    /* Insert entries with large content to ensure overflow pages exist */
    char big[8192];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    append_msg(db, sid, ROLE_USER, big, STOP_REASON_NONE, NULL, 0);
    append_msg(db, sid, ROLE_ASSISTANT, big, STOP_REASON_STOP, NULL, 0);

    /* The plan reads sizes, not content: the estimate reflects it. */
    Config cfg = {0};
    cfg.context_window = 128000;
    ContextPlan plan = {0};
    int rc = context_plan(db, sid, &cfg, 0, &plan);
    assert(rc == 0);
    assert(plan.count == 2);
    /* Verify token_estimate reflects the large content (not zero/default) */
    assert(plan.entries[0].tokens >= 100);
    assert(plan.entries[1].tokens >= 100);

    context_plan_free(&plan);
    db_close(db);
}

static void test_context_limits_window(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);

    sqlite3_exec(db,
        "INSERT INTO agents(name) VALUES ('a-id'),('a-null');"
        "INSERT INTO providers(name, base_url) VALUES ('p','http://p');"
        "INSERT INTO models(id, provider_name, model, context_window) VALUES"
        "  ('m-big','p','vendor/big',200000),"
        "  ('m-null','p','vendor/null',NULL);"
        "INSERT INTO agent_models(agent_name, model_id, pos) VALUES"
        "  ('a-id','m-big',0),"
        "  ('a-null','m-null',0);",
        NULL, NULL, NULL);

    ContextLimits l;
    context_limits(db, "a-id", 128000, &l);
    assert(l.window == 200000);
    context_limits(db, "a-null", 128000, &l);
    assert(l.window == 128000);
    context_limits(db, "ghost", 128000, &l);
    assert(l.window == 128000);
    context_limits(db, NULL, 111, &l);
    assert(l.window == 111);

    db_close(db);
}

int main(void) {
    TEST_INIT();
    printf("test_context_plan:\n");
    RUN(test_basic_plan);
    RUN(test_v28_filtering);
    RUN(test_cut_point);
    RUN(test_multi_user_turn_not_split);
    RUN(test_plan_no_content_access);
    RUN(test_context_limits_window);
    return 0;
}
