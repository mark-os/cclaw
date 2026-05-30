/* T181: integration test — empty response.
 * Mock returns finish_reason:"stop" + content:null; verify agent treats it as
 * valid end-of-turn (content is optional). Prior tool-call entries survive.
 * Cites: V29, V36. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <curl/curl.h>
#include "agent.h"
#include "db.h"
#include "config.h"
#include "mock_server.h"
static int s_port;

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

static char *mock_dispatch(const char *name, const char *arguments, void *user_data) {
    (void)user_data; (void)arguments;
    if (strcmp(name, "get_weather") == 0)
        return strdup("{\"temp\":22,\"condition\":\"sunny\"}");
    return strdup("ok");
}

/* T181: tool call then empty final response — valid, prior entries survive */
static void test_empty_response_after_tool(void) {
    TEST(empty_response_after_tool);

    mock_server_reset();

    mock_server_load("test/fixtures/empty_response_after_tool.json");

    sqlite3 *db = db_open(":memory:");
    if (!db) { FAIL("db_open failed"); }

    int64_t sid = session_create(db, "empty_resp_test", NULL, -1, 0);
    if (sid < 0) { db_close(db); FAIL("session_create failed"); }

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", s_port);

    Config cfg = {0};
    cfg.provider.base_url = base_url;
    cfg.provider.api_key = "mock-key";
    cfg.provider.model = "mock-model";
    cfg.provider.max_tokens = 256;
    cfg.provider.context_window = 128000;
    cfg.max_iterations = 10;
    cfg.system_prompt = "You are a helpful assistant.";

    ToolSchema tools[1] = {{
        .name = "get_weather",
        .description = "Get weather",
        .parameters_json = "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\"}},\"required\":[\"city\"]}"
    }};

    Message user_msg = {.role = ROLE_USER, .content = "Weather in NYC?"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;
    ctx.tools = tools;
    ctx.tool_count = 1;

    int rc = agent_run(&ctx);
    if (rc != 0) { db_close(db); FAIL("expected agent_run to return 0"); }

    /* Verify: user → assistant(tool_call) → tool_result → assistant(empty, stop) */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    if (!entries || count != 4) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected 4 entries, got %d", count);
        entry_branch_free(entries, count);
        db_close(db); FAIL(msg);
    }

    /* Prior tool-call content survives */
    if (entries[1].message.role != ROLE_ASSISTANT || entries[1].message.tool_call_count != 1) {
        entry_branch_free(entries, count); db_close(db);
        FAIL("entry[1] should be assistant with tool_call");
    }
    if (!strstr(entries[2].message.tool_result->content, "sunny")) {
        entry_branch_free(entries, count); db_close(db);
        FAIL("tool_result should contain weather data");
    }

    /* Final entry: stop, not error */
    if (entries[3].message.stop_reason != STOP_REASON_STOP) {
        entry_branch_free(entries, count); db_close(db);
        FAIL("final entry stop_reason should be STOP");
    }

    entry_branch_free(entries, count);
    db_close(db);
    PASS();
}

/* T181: standalone empty response — also valid (content is optional) */
static void test_empty_response_standalone(void) {
    TEST(empty_response_standalone);

    mock_server_reset();

    mock_server_enqueue(200,
        "{\"id\":\"c1\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":null},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":10,"
        "\"completion_tokens\":0,\"total_tokens\":10}}");

    sqlite3 *db = db_open(":memory:");
    if (!db) { FAIL("db_open failed"); }

    int64_t sid = session_create(db, "empty_standalone", NULL, -1, 0);
    if (sid < 0) { db_close(db); FAIL("session_create failed"); }

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", s_port);

    Config cfg = {0};
    cfg.provider.base_url = base_url;
    cfg.provider.api_key = "mock-key";
    cfg.provider.model = "mock-model";
    cfg.provider.max_tokens = 256;
    cfg.provider.context_window = 128000;
    cfg.max_iterations = 10;
    cfg.system_prompt = "You are a helpful assistant.";

    Message user_msg = {.role = ROLE_USER, .content = "Hello"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;
    ctx.tools = NULL;
    ctx.tool_count = 0;

    int rc = agent_run(&ctx);
    if (rc != 0) { db_close(db); FAIL("expected agent_run to return 0"); }

    /* Verify: user + assistant (empty content, stop) */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    if (!entries || count != 2) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected 2 entries, got %d", count);
        entry_branch_free(entries, count);
        db_close(db); FAIL(msg);
    }

    if (entries[1].message.stop_reason != STOP_REASON_STOP) {
        entry_branch_free(entries, count); db_close(db);
        FAIL("stop_reason should be STOP, not ERROR");
    }

    entry_branch_free(entries, count);
    db_close(db);
    PASS();
}

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    s_port = mock_server_start();
    printf("--- test_integration_empty_response (T181) ---\n");
    test_empty_response_after_tool();
    test_empty_response_standalone();
    printf("%d/%d passed\n", tests_passed, tests_run);
    curl_global_cleanup();
    mock_server_stop();
    return tests_passed == tests_run ? 0 : 1;
}
