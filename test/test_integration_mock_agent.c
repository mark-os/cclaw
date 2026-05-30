/* T126: integration test — agent loop with mock LLM server.
 * Multi-turn tool call sequence: mock returns tool_call → agent dispatches →
 * mock returns final. Verify entries in DB match expected flow. */
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

/* Mock tool dispatch — returns canned results by tool name */
static char *mock_dispatch(const char *name, const char *arguments, void *user_data) {
    (void)user_data;
    if (strcmp(name, "get_weather") == 0) {
        return strdup("{\"temp\":22,\"condition\":\"sunny\"}");
    }
    if (strcmp(name, "get_time") == 0) {
        return strdup("2026-05-25T12:00:00Z");
    }
    char *err = malloc(64);
    snprintf(err, 64, "error: unknown tool '%s'", name);
    (void)arguments;
    return err;
}

/* Helper: build a mock LLM response with a single tool call */
static const char *mock_tool_call_response(const char *tool_name, const char *args) {
    static char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"id\":\"chatcmpl-mock\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":null,\"tool_calls\":[{\"id\":\"call_001\",\"type\":\"function\","
        "\"function\":{\"name\":\"%s\",\"arguments\":\"%s\"}}]},"
        "\"finish_reason\":\"tool_calls\"}],\"usage\":{\"prompt_tokens\":10,"
        "\"completion_tokens\":5,\"total_tokens\":15}}",
        tool_name, args);
    return buf;
}

/* Helper: build a mock LLM final text response */
static const char *mock_final_response(const char *text) {
    static char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"id\":\"chatcmpl-mock2\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"%s\"},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":20,"
        "\"completion_tokens\":10,\"total_tokens\":30}}",
        text);
    return buf;
}

/* T126: single tool call → final response */
static void test_mock_agent_single_tool(void) {
    TEST(mock_agent_single_tool);

    mock_server_reset();

    /* Enqueue: first call returns tool_call, second returns final text */
    mock_server_enqueue(200, mock_tool_call_response("get_weather", "{\\\"city\\\":\\\"Tokyo\\\"}"));
    mock_server_enqueue(200, mock_final_response("The weather in Tokyo is 22C and sunny."));

    sqlite3 *db = db_open(":memory:");
    if (!db) { FAIL("db_open failed"); }

    int64_t sid = session_create(db, "mock_test", NULL, -1, 0);
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
        .description = "Get weather for a city",
        .parameters_json = "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\"}},\"required\":[\"city\"]}"
    }};

    /* Append user message */
    Message user_msg = {.role = ROLE_USER, .content = "What is the weather in Tokyo?"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;
    ctx.tools = tools;
    ctx.tool_count = 1;

    int rc = agent_run(&ctx);
    if (rc != 0) { db_close(db); FAIL("agent_run failed"); }

    /* Verify entries: user → assistant(tool_call) → tool_result → assistant(final) */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    if (!entries || count != 4) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected 4 entries, got %d", count);
        entry_branch_free(entries, count);
        db_close(db); FAIL(msg);
    }

    /* Entry 0: user */
    if (entries[0].message.role != ROLE_USER) {
        entry_branch_free(entries, count); db_close(db);
        FAIL("entry[0] should be user");
    }

    /* Entry 1: assistant with tool_calls */
    if (entries[1].message.role != ROLE_ASSISTANT || entries[1].message.tool_call_count != 1) {
        entry_branch_free(entries, count); db_close(db);
        FAIL("entry[1] should be assistant with 1 tool_call");
    }
    if (strcmp(entries[1].message.tool_calls[0].name, "get_weather") != 0) {
        entry_branch_free(entries, count); db_close(db);
        FAIL("tool_call name should be get_weather");
    }

    /* Entry 2: tool result */
    if (entries[2].message.role != ROLE_TOOL) {
        entry_branch_free(entries, count); db_close(db);
        FAIL("entry[2] should be tool_result");
    }
    if (!entries[2].message.tool_result ||
        !strstr(entries[2].message.tool_result->content, "sunny")) {
        entry_branch_free(entries, count); db_close(db);
        FAIL("tool_result should contain weather data");
    }

    /* Entry 3: final assistant */
    if (entries[3].message.role != ROLE_ASSISTANT || entries[3].message.tool_call_count != 0) {
        entry_branch_free(entries, count); db_close(db);
        FAIL("entry[3] should be final assistant with no tool_calls");
    }
    if (!entries[3].message.content || !strstr(entries[3].message.content, "22C")) {
        entry_branch_free(entries, count); db_close(db);
        FAIL("final response should contain temperature");
    }

    /* Verify mock received exactly 2 requests */
    if (mock_server_request_count() != 2) {
        entry_branch_free(entries, count); db_close(db);
        FAIL("expected 2 LLM requests");
    }

    entry_branch_free(entries, count);
    db_close(db);
    PASS();
}

/* T126: two consecutive tool calls → final response (multi-turn) */
static void test_mock_agent_multi_tool(void) {
    TEST(mock_agent_multi_tool);

    mock_server_reset();

    /* Turn 1: LLM requests get_weather */
    mock_server_enqueue(200, mock_tool_call_response("get_weather", "{\\\"city\\\":\\\"Tokyo\\\"}"));
    /* Turn 2: LLM requests get_time */
    mock_server_enqueue(200, mock_tool_call_response("get_time", "{}"));
    /* Turn 3: LLM produces final answer */
    mock_server_enqueue(200, mock_final_response("Tokyo is 22C at 12:00 UTC."));

    sqlite3 *db = db_open(":memory:");
    if (!db) { FAIL("db_open failed"); }

    int64_t sid = session_create(db, "multi_tool_test", NULL, -1, 0);
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

    ToolSchema tools[2] = {
        {.name = "get_weather", .description = "Get weather",
         .parameters_json = "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\"}},\"required\":[\"city\"]}"},
        {.name = "get_time", .description = "Get current time",
         .parameters_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}"}
    };

    Message user_msg = {.role = ROLE_USER, .content = "Weather and time in Tokyo?"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;
    ctx.tools = tools;
    ctx.tool_count = 2;

    int rc = agent_run(&ctx);
    if (rc != 0) { db_close(db); FAIL("agent_run failed"); }

    /* Expected: user → asst(tool) → tool_result → asst(tool) → tool_result → asst(final) = 6 entries */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    if (!entries || count != 6) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected 6 entries, got %d", count);
        entry_branch_free(entries, count);
        db_close(db); FAIL(msg);
    }

    /* Verify structure */
    if (entries[0].message.role != ROLE_USER) {
        entry_branch_free(entries, count); db_close(db);
        FAIL("entry[0] should be user");
    }
    if (entries[1].message.role != ROLE_ASSISTANT || entries[1].message.tool_call_count == 0) {
        entry_branch_free(entries, count); db_close(db);
        FAIL("entry[1] should be assistant with tool_call");
    }
    if (entries[2].message.role != ROLE_TOOL) {
        entry_branch_free(entries, count); db_close(db);
        FAIL("entry[2] should be tool_result");
    }
    if (entries[3].message.role != ROLE_ASSISTANT || entries[3].message.tool_call_count == 0) {
        entry_branch_free(entries, count); db_close(db);
        FAIL("entry[3] should be assistant with tool_call");
    }
    if (entries[4].message.role != ROLE_TOOL) {
        entry_branch_free(entries, count); db_close(db);
        FAIL("entry[4] should be tool_result");
    }
    if (entries[5].message.role != ROLE_ASSISTANT || entries[5].message.tool_call_count != 0) {
        entry_branch_free(entries, count); db_close(db);
        FAIL("entry[5] should be final assistant");
    }
    if (!entries[5].message.content || !strstr(entries[5].message.content, "12:00")) {
        entry_branch_free(entries, count); db_close(db);
        FAIL("final response should contain time");
    }

    /* Verify 3 LLM requests total */
    if (mock_server_request_count() != 3) {
        entry_branch_free(entries, count); db_close(db);
        FAIL("expected 3 LLM requests");
    }

    entry_branch_free(entries, count);
    db_close(db);
    PASS();
}

/* T126: tool returns error → agent still completes (V10) */
static void test_mock_agent_tool_error(void) {
    TEST(mock_agent_tool_error);

    mock_server_reset();

    /* LLM calls unknown_tool, dispatch returns error, LLM produces final */
    mock_server_enqueue(200, mock_tool_call_response("unknown_tool", "{}"));
    mock_server_enqueue(200, mock_final_response("Sorry, I could not complete that."));

    sqlite3 *db = db_open(":memory:");
    if (!db) { FAIL("db_open failed"); }

    int64_t sid = session_create(db, "error_test", NULL, -1, 0);
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
        .name = "get_weather", .description = "Get weather",
        .parameters_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}"
    }};

    Message user_msg = {.role = ROLE_USER, .content = "Do something"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;
    ctx.tools = tools;
    ctx.tool_count = 1;

    int rc = agent_run(&ctx);
    if (rc != 0) { db_close(db); FAIL("agent_run should succeed despite tool error"); }

    /* Verify tool_result contains error */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    if (!entries || count < 4) {
        entry_branch_free(entries, count); db_close(db);
        FAIL("expected at least 4 entries");
    }

    /* Entry 2 should be tool_result with error */
    if (entries[2].message.role != ROLE_TOOL) {
        entry_branch_free(entries, count); db_close(db);
        FAIL("entry[2] should be tool_result");
    }
    if (!entries[2].message.tool_result ||
        !strstr(entries[2].message.tool_result->content, "error")) {
        entry_branch_free(entries, count); db_close(db);
        FAIL("tool_result should contain error");
    }

    entry_branch_free(entries, count);
    db_close(db);
    PASS();
}

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    s_port = mock_server_start();
    printf("--- test_integration_mock_agent (T126) ---\n");
    test_mock_agent_single_tool();
    test_mock_agent_multi_tool();
    test_mock_agent_tool_error();
    printf("%d/%d passed\n", tests_passed, tests_run);
    curl_global_cleanup();
    mock_server_stop();
    return tests_passed == tests_run ? 0 : 1;
}
