/* T181: integration test — empty response.
 * Mock returns finish_reason:"stop" + content:null; verify behavior depends on
 * whether prior assistant content exists in the turn.
 * - With prior tool-call content: valid end-of-turn (return 0)
 * - Without prior content: error entry written (return -1)
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

/* T181: tool call succeeds, then LLM returns stop + null content → valid end */
static void test_empty_response_after_tool(void) {
    TEST(empty_response_after_tool_is_valid);

    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start failed");

    /* First response: tool call */
    mock_server_enqueue(200,
        "{\"id\":\"c1\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":null,\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\","
        "\"function\":{\"name\":\"get_weather\",\"arguments\":\"{\\\"city\\\":\\\"NYC\\\"}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}],\"usage\":{\"prompt_tokens\":10,"
        "\"completion_tokens\":5,\"total_tokens\":15}}");

    /* Second response: stop + null content (valid — prior content exists) */
    mock_server_enqueue(200,
        "{\"id\":\"c2\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":null},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":20,"
        "\"completion_tokens\":0,\"total_tokens\":20}}");

    sqlite3 *db = db_open(":memory:");
    if (!db) { mock_server_stop(); FAIL("db_open failed"); }

    int64_t sid = session_create(db, "empty_resp_test", NULL, -1, 0);
    if (sid < 0) { db_close(db); mock_server_stop(); FAIL("session_create failed"); }

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);

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
    /* Should succeed — empty final response is valid when prior content exists */
    if (rc != 0) { db_close(db); mock_server_stop(); FAIL("expected agent_run to return 0"); }

    /* Verify entries in DB:
     * 0: user
     * 1: assistant (tool_call) — prior content survives
     * 2: tool_result
     * 3: assistant (empty content, stop_reason=stop — valid end) */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    if (!entries || count != 4) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected 4 entries, got %d", count);
        entry_branch_free(entries, count);
        db_close(db); mock_server_stop(); FAIL(msg);
    }

    /* Entry 1: assistant with tool_call (prior content survives) */
    if (entries[1].message.role != ROLE_ASSISTANT || entries[1].message.tool_call_count != 1) {
        entry_branch_free(entries, count); db_close(db); mock_server_stop();
        FAIL("entry[1] should be assistant with 1 tool_call");
    }

    /* Entry 2: tool result with weather data */
    if (entries[2].message.role != ROLE_TOOL) {
        entry_branch_free(entries, count); db_close(db); mock_server_stop();
        FAIL("entry[2] should be tool_result");
    }
    if (!entries[2].message.tool_result ||
        !strstr(entries[2].message.tool_result->content, "sunny")) {
        entry_branch_free(entries, count); db_close(db); mock_server_stop();
        FAIL("tool_result should contain weather data");
    }

    /* Entry 3: final assistant — stop reason is STOP (not error) */
    if (entries[3].message.role != ROLE_ASSISTANT) {
        entry_branch_free(entries, count); db_close(db); mock_server_stop();
        FAIL("entry[3] should be assistant");
    }
    if (entries[3].message.stop_reason != STOP_REASON_STOP) {
        char msg[64];
        snprintf(msg, sizeof(msg), "entry[3] stop_reason=%d, expected STOP(%d)",
                 entries[3].message.stop_reason, STOP_REASON_STOP);
        entry_branch_free(entries, count); db_close(db); mock_server_stop();
        FAIL(msg);
    }

    entry_branch_free(entries, count);
    db_close(db);
    mock_server_stop();
    PASS();
}

/* T181: standalone empty response (no prior content) → error */
static void test_empty_response_standalone(void) {
    TEST(empty_response_standalone_is_error);

    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start failed");

    /* Single response: stop + null content, no prior content */
    mock_server_enqueue(200,
        "{\"id\":\"c1\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":null},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":10,"
        "\"completion_tokens\":0,\"total_tokens\":10}}");

    sqlite3 *db = db_open(":memory:");
    if (!db) { mock_server_stop(); FAIL("db_open failed"); }

    int64_t sid = session_create(db, "empty_standalone", NULL, -1, 0);
    if (sid < 0) { db_close(db); mock_server_stop(); FAIL("session_create failed"); }

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);

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
    if (rc != -1) { db_close(db); mock_server_stop(); FAIL("expected agent_run to return -1"); }

    /* Verify: user + error assistant entry */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    if (!entries || count != 2) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected 2 entries, got %d", count);
        entry_branch_free(entries, count);
        db_close(db); mock_server_stop(); FAIL(msg);
    }

    if (entries[1].message.stop_reason != STOP_REASON_ERROR) {
        entry_branch_free(entries, count); db_close(db); mock_server_stop();
        FAIL("entry[1] stop_reason should be ERROR");
    }

    entry_branch_free(entries, count);
    db_close(db);
    mock_server_stop();
    PASS();
}

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    printf("--- test_integration_empty_response (T181) ---\n");
    test_empty_response_after_tool();
    test_empty_response_standalone();
    printf("%d/%d passed\n", tests_passed, tests_run);
    curl_global_cleanup();
    return tests_passed == tests_run ? 0 : 1;
}
