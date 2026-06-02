/* Integration test: SSE streaming response parsing.
 * Verifies that when stream=1, the agent sends "stream":true and correctly
 * reconstructs the response from SSE chunks for DB storage. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
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
    char *r = malloc(64);
    snprintf(r, 64, "error: unknown tool '%s'", name);
    return r;
}

/* Test: streaming request includes "stream":true */
static void test_stream_request_format(void) {
    TEST(stream_request_format);

    /* SSE response: content arrives in chunks */
    const char *sse_body =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\" world\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}],"
               "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":2,\"total_tokens\":12}}\n\n"
        "data: [DONE]\n\n";

    mock_server_reset();
    mock_server_enqueue(200, sse_body);

    /* Setup agent */
    sqlite3 *db = db_open_agent("/tmp/cclaw_stream_test.db");
    assert(db);
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Config cfg = {0};
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", s_port);
    cfg.provider.base_url = url;
    cfg.provider.api_key = "test-key";
    cfg.provider.model = "test-model";
    cfg.provider.context_window = 128000;
    cfg.max_iterations = 5;
    cfg.stream = 1;
    cfg.context_threshold = 0.6f;
    cfg.compaction_target = 0.3f;
    cfg.compaction = 0;

    /* Insert system + user message */
    Message sys = {.role = ROLE_SYSTEM, .content = "You are helpful."};
    entry_append(db, sid, &sys);
    Message user = {.role = ROLE_USER, .content = "Say hello"};
    entry_append(db, sid, &user);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;
    ctx.dispatch_data = NULL;
    ctx.tools = NULL;
    ctx.tool_count = 0;

    int rc = agent_run(&ctx);
    if (rc != 0) FAIL("agent_run failed");

    /* Verify request had "stream":true */
    const char *req_body = mock_server_last_request_body();
    if (!req_body) FAIL("no request captured");
    if (!strstr(req_body, "\"stream\":true")) FAIL("request missing stream:true");

    /* Verify response was stored correctly in DB */
    char *resp = get_response_text(db, sid);
    if (!resp) FAIL("no response stored");
    if (strcmp(resp, "Hello world") != 0) {
        printf("got '%s', ", resp);
        free(resp);
        FAIL("content mismatch");
    }
    free(resp);

    db_close(db);
    remove("/tmp/cclaw_stream_test.db");
    PASS();
}

/* Test: streaming with tool_calls reconstructs properly */
static void test_stream_tool_calls(void) {
    TEST(stream_tool_calls);

    /* SSE response with tool_calls */
    const char *sse_body =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_123\","
               "\"type\":\"function\",\"function\":{\"name\":\"get_w\",\"arguments\":\"\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
               "\"function\":{\"arguments\":\"{\\\"ci\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
               "\"function\":{\"arguments\":\"ty\\\"\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
               "\"function\":{\"arguments\":\":\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
               "\"function\":{\"arguments\":\"\\\"SF\\\"\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
               "\"function\":{\"arguments\":\"}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}],"
               "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5,\"total_tokens\":15}}\n\n"
        "data: [DONE]\n\n";

    /* After tool call, mock returns final response (non-streaming to simplify) */
    const char *final_resp =
        "{\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"It's sunny in SF!\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":5,\"total_tokens\":25}}";

    mock_server_reset();
    mock_server_enqueue(200, sse_body);
    mock_server_enqueue(200, final_resp);

    sqlite3 *db = db_open_agent("/tmp/cclaw_stream_tc_test.db");
    assert(db);
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Config cfg = {0};
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", s_port);
    cfg.provider.base_url = url;
    cfg.provider.api_key = "test-key";
    cfg.provider.model = "test-model";
    cfg.provider.context_window = 128000;
    cfg.max_iterations = 5;
    cfg.stream = 1;
    cfg.context_threshold = 0.6f;
    cfg.compaction_target = 0.3f;
    cfg.compaction = 0;

    Message sys = {.role = ROLE_SYSTEM, .content = "You are helpful."};
    entry_append(db, sid, &sys);
    Message user = {.role = ROLE_USER, .content = "Weather in SF?"};
    entry_append(db, sid, &user);

    ToolSchema tools[] = {{
        .name = "get_w",
        .description = "Get weather",
        .parameters_json = "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\"}}}"
    }};

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;
    ctx.dispatch_data = NULL;
    ctx.tools = tools;
    ctx.tool_count = 1;

    int rc = agent_run(&ctx);
    if (rc != 0) FAIL("agent_run failed");

    /* Verify final response */
    char *resp = get_response_text(db, sid);
    if (!resp) FAIL("no response stored");
    if (strcmp(resp, "It's sunny in SF!") != 0) {
        printf("got '%s', ", resp);
        free(resp);
        FAIL("content mismatch");
    }
    free(resp);

    db_close(db);
    remove("/tmp/cclaw_stream_tc_test.db");
    PASS();
}

/* Test: non-streaming still works when stream=0 */
static void test_no_stream(void) {
    TEST(no_stream_still_works);

    const char *resp_body =
        "{\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"Hi there!\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":2,\"total_tokens\":7}}";

    mock_server_reset();
    mock_server_enqueue(200, resp_body);

    sqlite3 *db = db_open_agent("/tmp/cclaw_nostream_test.db");
    assert(db);
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Config cfg = {0};
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", s_port);
    cfg.provider.base_url = url;
    cfg.provider.api_key = "test-key";
    cfg.provider.model = "test-model";
    cfg.provider.context_window = 128000;
    cfg.max_iterations = 5;
    cfg.stream = 0;
    cfg.context_threshold = 0.6f;
    cfg.compaction_target = 0.3f;
    cfg.compaction = 0;

    Message sys = {.role = ROLE_SYSTEM, .content = "You are helpful."};
    entry_append(db, sid, &sys);
    Message user = {.role = ROLE_USER, .content = "Hello"};
    entry_append(db, sid, &user);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;
    ctx.dispatch_data = NULL;
    ctx.tools = NULL;
    ctx.tool_count = 0;

    int rc = agent_run(&ctx);
    if (rc != 0) FAIL("agent_run failed");

    /* Verify request does NOT have "stream":true */
    const char *req_body = mock_server_last_request_body();
    if (!req_body) FAIL("no request captured");
    if (strstr(req_body, "\"stream\"")) FAIL("non-streaming request has stream field");

    char *resp = get_response_text(db, sid);
    if (!resp || strcmp(resp, "Hi there!") != 0) {
        if (resp) free(resp);
        FAIL("content mismatch");
    }
    free(resp);

    db_close(db);
    remove("/tmp/cclaw_nostream_test.db");
    PASS();
}

int main(void) {
    printf("test_integration_streaming:\n");

    s_port = mock_server_start();
    if (s_port < 0) {
        printf("FAIL: could not start mock server\n");
        return 1;
    }

    test_stream_request_format();
    test_stream_tool_calls();
    test_no_stream();

    mock_server_stop();

    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
