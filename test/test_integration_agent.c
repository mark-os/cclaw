#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "agent.h"
#include "db.h"
#include "config.h"
#include "llm.h"
#include "http.h"
#include <curl/curl.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)
#define SKIP(msg) do { tests_passed++; printf("SKIP: %s\n", msg); return; } while(0)

static const char *get_api_key(void) {
    return getenv("OPENROUTER_API_KEY");
}

/* Tool dispatch: returns canned result for get_time */
static char *mock_dispatch(const char *name, const char *arguments, void *user_data) {
    (void)arguments; (void)user_data;
    if (strcmp(name, "get_time") == 0) {
        return strdup("2026-05-21T21:00:00Z");
    }
    char *err = malloc(64);
    snprintf(err, 64, "error: unknown tool '%s'", name);
    return err;
}

/* T53: full agent loop — prompt forces tool call, dispatch returns result,
 * agent produces final answer. Verifies V10 (tool dispatch works, loop continues). */
static void test_agent_loop_end_to_end(void) {
    TEST(agent_loop_end_to_end);
    const char *key = get_api_key();
    if (!key) SKIP("OPENROUTER_API_KEY not set");

    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open failed");

    int64_t sid = session_create(db, "integration_test", NULL);
    if (sid < 0) { db_close(db); FAIL("session_create failed"); }

    Config cfg = {0};
    cfg.provider.base_url = "https://openrouter.ai/api/v1";
    cfg.provider.api_key = (char *)key;
    cfg.provider.model = "deepseek/deepseek-v4-flash";
    cfg.provider.max_tokens = 256;
    cfg.provider.context_window = 128000;
    cfg.max_iterations = 5;
    cfg.system_prompt = "You must use the get_time tool to answer time questions. Always call the tool first.";

    ToolSchema tools[1] = {{
        .name = "get_time",
        .description = "Get the current UTC time",
        .parameters_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}"
    }};

    /* Append user message */
    Message user_msg = {.role = ROLE_USER, .content = "What time is it right now?"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;
    ctx.tools = tools;
    ctx.tool_count = 1;

    int rc = agent_run(&ctx);
    if (rc != 0) { db_close(db); FAIL("agent_run failed (expected 0)"); }

    /* Verify session has entries: user → assistant(tool_call) → tool → assistant(final) */
    int entry_count = 0;
    Entry *entries = session_get_branch(db, sid, &entry_count);
    if (!entries || entry_count < 4) {
        entry_branch_free(entries, entry_count);
        db_close(db);
        FAIL("expected at least 4 entries (user, asst+tool_call, tool_result, asst_final)");
    }

    /* Entry 0: user */
    if (entries[0].message.role != ROLE_USER) {
        entry_branch_free(entries, entry_count); db_close(db);
        FAIL("entry[0] should be user");
    }

    /* Entry 1: assistant with tool_calls */
    if (entries[1].message.role != ROLE_ASSISTANT || entries[1].message.tool_call_count == 0) {
        entry_branch_free(entries, entry_count); db_close(db);
        FAIL("entry[1] should be assistant with tool_calls");
    }
    if (strcmp(entries[1].message.tool_calls[0].name, "get_time") != 0) {
        entry_branch_free(entries, entry_count); db_close(db);
        FAIL("tool call should be get_time");
    }

    /* Entry 2: tool result */
    if (entries[2].message.role != ROLE_TOOL) {
        entry_branch_free(entries, entry_count); db_close(db);
        FAIL("entry[2] should be tool result");
    }

    /* Last entry: assistant final (no tool_calls) */
    Entry *last = &entries[entry_count - 1];
    if (last->message.role != ROLE_ASSISTANT) {
        entry_branch_free(entries, entry_count); db_close(db);
        FAIL("last entry should be assistant");
    }
    if (last->message.tool_call_count != 0) {
        entry_branch_free(entries, entry_count); db_close(db);
        FAIL("final assistant should have no tool_calls");
    }
    if (!last->message.content || strlen(last->message.content) == 0) {
        entry_branch_free(entries, entry_count); db_close(db);
        FAIL("final assistant should have content");
    }

    entry_branch_free(entries, entry_count);
    db_close(db);
    PASS();
}

/* V10: tool that returns error string — agent loop should still complete */
static char *error_dispatch(const char *name, const char *arguments, void *user_data) {
    (void)name; (void)arguments; (void)user_data;
    return strdup("error: tool execution failed");
}

static void test_agent_loop_tool_error_recovery(void) {
    TEST(agent_loop_tool_error_recovery);
    const char *key = get_api_key();
    if (!key) SKIP("OPENROUTER_API_KEY not set");

    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open failed");

    int64_t sid = session_create(db, "error_test", NULL);
    if (sid < 0) { db_close(db); FAIL("session_create failed"); }

    Config cfg = {0};
    cfg.provider.base_url = "https://openrouter.ai/api/v1";
    cfg.provider.api_key = (char *)key;
    cfg.provider.model = "deepseek/deepseek-v4-flash";
    cfg.provider.max_tokens = 256;
    cfg.provider.context_window = 128000;
    cfg.max_iterations = 10;
    cfg.system_prompt = "You have a get_time tool. Call it once. If it returns an error, apologize and say you cannot get the time. Do NOT retry the tool.";

    ToolSchema tools[1] = {{
        .name = "get_time",
        .description = "Get the current UTC time",
        .parameters_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}"
    }};

    Message user_msg = {.role = ROLE_USER, .content = "What time is it?"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = error_dispatch;
    ctx.tools = tools;
    ctx.tool_count = 1;

    /* V10: even with tool error, agent should complete (return 0) —
     * the error result is fed back and the LLM produces a final text response */
    int rc = agent_run(&ctx);
    if (rc != 0) { db_close(db); FAIL("agent_run should succeed even with tool error"); }

    /* Verify session has at least: user → assistant(tool_call) → tool → assistant(final) */
    int entry_count = 0;
    Entry *entries = session_get_branch(db, sid, &entry_count);
    if (!entries || entry_count < 3) {
        entry_branch_free(entries, entry_count); db_close(db);
        FAIL("expected at least 3 entries");
    }

    /* Last entry must be a final assistant response (no tool_calls) */
    Entry *last = &entries[entry_count - 1];
    if (last->message.role != ROLE_ASSISTANT || last->message.tool_call_count != 0) {
        entry_branch_free(entries, entry_count); db_close(db);
        FAIL("last entry should be final assistant response");
    }

    entry_branch_free(entries, entry_count);
    db_close(db);
    PASS();
}

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    printf("--- test_integration_agent ---\n");
    test_agent_loop_end_to_end();
    test_agent_loop_tool_error_recovery();
    printf("%d/%d passed\n", tests_passed, tests_run);
    curl_global_cleanup();
    return tests_passed == tests_run ? 0 : 1;
}
