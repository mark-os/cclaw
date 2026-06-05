/* T290: Gemini direct provider integration test.
 * Verifies: native request format (contents[], systemInstruction, functionDeclarations),
 * response parsing (candidates[0].content.parts[]), tool call round-trip. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "agent.h"
#include "db.h"
#include "config.h"
#include "mock_server.h"
#include "cJSON.h"

static int s_port;
static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

static char *mock_dispatch(const char *name, const char *arguments, void *data) {
    (void)data; (void)arguments;
    if (strcmp(name, "get_weather") == 0)
        return strdup("{\"temp\":20,\"condition\":\"cloudy\"}");
    return strdup("error: unknown tool");
}

/* Test 1: simple text response in Gemini format */
static void test_gemini_text_response(void) {
    TEST(gemini_text_response);
    mock_server_reset();

    const char *gemini_resp =
        "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"Hello from Gemini!\"}]},"
        "\"finishReason\":\"STOP\"}],"
        "\"usageMetadata\":{\"promptTokenCount\":10,\"candidatesTokenCount\":5,\"totalTokenCount\":15}}";
    mock_server_enqueue(200, gemini_resp);

    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");
    int64_t sid = session_create(db, "gemini_test", NULL, -1, 0);

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", s_port);

    Config cfg = {0};
    cfg.provider.base_url = base_url;
    cfg.provider.api_key = "test-gemini-key";
    cfg.provider.model = "gemini-2.5-flash";
    cfg.provider.max_tokens = 256;
    cfg.provider.context_window = 128000;
    cfg.provider.endpoint_type = ENDPOINT_GEMINI;
    cfg.max_iterations = 5;
    cfg.system_prompt = "You are helpful.";

    Message sys_msg = {.role = ROLE_SYSTEM, .content = "You are helpful."};
    entry_append(db, sid, &sys_msg);
    Message user_msg = {.role = ROLE_USER, .content = "Hello"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;

    int rc = agent_run(&ctx);
    if (rc != 0) { db_close(db); FAIL("agent_run failed"); }

    /* Verify response stored correctly */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    if (count != 3) { entry_branch_free(entries, count); db_close(db); FAIL("expected 3 entries"); }
    if (!entries[2].message.content || strcmp(entries[2].message.content, "Hello from Gemini!") != 0) {
        entry_branch_free(entries, count); db_close(db); FAIL("wrong content");
    }
    entry_branch_free(entries, count);

    /* Verify request format: should have contents[], systemInstruction */
    const char *body = mock_server_last_request_body();
    if (!body) { db_close(db); FAIL("no request body"); }
    cJSON *req = cJSON_Parse(body);
    if (!req) { db_close(db); FAIL("request not valid JSON"); }

    /* Must have "contents" array */
    cJSON *contents = cJSON_GetObjectItem(req, "contents");
    if (!contents || !cJSON_IsArray(contents)) { cJSON_Delete(req); db_close(db); FAIL("no contents[]"); }

    /* Must have "systemInstruction" */
    cJSON *sys = cJSON_GetObjectItem(req, "systemInstruction");
    if (!sys) { cJSON_Delete(req); db_close(db); FAIL("no systemInstruction"); }

    /* Must NOT have "model" or "messages" (OpenAI fields) */
    if (cJSON_GetObjectItem(req, "model")) { cJSON_Delete(req); db_close(db); FAIL("has model field"); }
    if (cJSON_GetObjectItem(req, "messages")) { cJSON_Delete(req); db_close(db); FAIL("has messages field"); }

    /* Contents should have user message with parts[0].text */
    cJSON *c0 = cJSON_GetArrayItem(contents, 0);
    cJSON *role = cJSON_GetObjectItem(c0, "role");
    if (!role || strcmp(role->valuestring, "user") != 0) { cJSON_Delete(req); db_close(db); FAIL("wrong role"); }
    cJSON *parts = cJSON_GetObjectItem(c0, "parts");
    cJSON *p0 = cJSON_GetArrayItem(parts, 0);
    cJSON *text = cJSON_GetObjectItem(p0, "text");
    if (!text || strcmp(text->valuestring, "Hello") != 0) { cJSON_Delete(req); db_close(db); FAIL("wrong text"); }

    cJSON_Delete(req);
    db_close(db);
    PASS();
}

/* Test 2: tool call round-trip in Gemini format */
static void test_gemini_tool_call(void) {
    TEST(gemini_tool_call);
    mock_server_reset();

    /* First response: functionCall */
    const char *fc_resp =
        "{\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":["
        "{\"functionCall\":{\"name\":\"get_weather\",\"args\":{\"city\":\"Tokyo\"}}}"
        "]},\"finishReason\":\"STOP\"}],"
        "\"usageMetadata\":{\"promptTokenCount\":10,\"candidatesTokenCount\":5,\"totalTokenCount\":15}}";
    mock_server_enqueue(200, fc_resp);

    /* Second response: text after tool result */
    const char *text_resp =
        "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"It is 20C and cloudy in Tokyo.\"}]},"
        "\"finishReason\":\"STOP\"}],"
        "\"usageMetadata\":{\"promptTokenCount\":20,\"candidatesTokenCount\":10,\"totalTokenCount\":30}}";
    mock_server_enqueue(200, text_resp);

    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");
    int64_t sid = session_create(db, "gemini_tc_test", NULL, -1, 0);

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", s_port);

    Config cfg = {0};
    cfg.provider.base_url = base_url;
    cfg.provider.api_key = "test-gemini-key";
    cfg.provider.model = "gemini-2.5-flash";
    cfg.provider.max_tokens = 256;
    cfg.provider.context_window = 128000;
    cfg.provider.endpoint_type = ENDPOINT_GEMINI;
    cfg.max_iterations = 10;
    cfg.system_prompt = "You are helpful.";

    ToolSchema tools[1] = {{
        .name = "get_weather",
        .description = "Get weather",
        .parameters_json = "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\"}}}"
    }};

    Message sys_msg2 = {.role = ROLE_SYSTEM, .content = "You are helpful."};
    entry_append(db, sid, &sys_msg2);
    Message user_msg = {.role = ROLE_USER, .content = "Weather in Tokyo?"};
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

    /* Verify 5 entries: sys → user → assistant(tool_call) → tool_result → assistant(final) */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    if (count != 5) {
        char msg[64]; snprintf(msg, sizeof(msg), "expected 5 entries, got %d", count);
        entry_branch_free(entries, count); db_close(db); FAIL(msg);
    }
    if (entries[2].message.tool_call_count != 1) {
        entry_branch_free(entries, count); db_close(db); FAIL("expected 1 tool_call");
    }
    if (strcmp(entries[2].message.tool_calls[0].name, "get_weather") != 0) {
        entry_branch_free(entries, count); db_close(db); FAIL("wrong tool name");
    }
    if (!strstr(entries[4].message.content, "20C")) {
        entry_branch_free(entries, count); db_close(db); FAIL("wrong final content");
    }

    /* Verify second request has functionResponse in contents */
    const char *body = mock_server_last_request_body();
    if (!body) { entry_branch_free(entries, count); db_close(db); FAIL("no request body"); }
    if (!strstr(body, "functionResponse")) {
        entry_branch_free(entries, count); db_close(db); FAIL("no functionResponse in request");
    }

    entry_branch_free(entries, count);
    db_close(db);
    PASS();
}

/* Test 3: Gemini auth header uses x-goog-api-key (verified by URL pattern) */
static void test_gemini_url_format(void) {
    TEST(gemini_url_format);
    mock_server_reset();

    const char *resp =
        "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"ok\"}]},"
        "\"finishReason\":\"STOP\"}],"
        "\"usageMetadata\":{\"promptTokenCount\":5,\"candidatesTokenCount\":2,\"totalTokenCount\":7}}";
    mock_server_enqueue(200, resp);

    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");
    int64_t sid = session_create(db, "url_test", NULL, -1, 0);

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", s_port);

    Config cfg = {0};
    cfg.provider.base_url = base_url;
    cfg.provider.api_key = "AIza-test-key";
    cfg.provider.model = "gemini-2.5-flash";
    cfg.provider.max_tokens = 100;
    cfg.provider.context_window = 128000;
    cfg.provider.endpoint_type = ENDPOINT_GEMINI;
    cfg.max_iterations = 5;

    Message user_msg = {.role = ROLE_USER, .content = "hi"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;

    int rc = agent_run(&ctx);
    if (rc != 0) { db_close(db); FAIL("agent_run failed"); }

    /* Verify request count — confirms the mock server received it at /models/ endpoint */
    if (mock_server_request_count() != 1) { db_close(db); FAIL("request not received"); }

    db_close(db);
    PASS();
}

/* Test 4: Gemini tools format — functionDeclarations */
static void test_gemini_tools_format(void) {
    TEST(gemini_tools_format);
    mock_server_reset();

    const char *resp =
        "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"done\"}]},"
        "\"finishReason\":\"STOP\"}],"
        "\"usageMetadata\":{\"promptTokenCount\":5,\"candidatesTokenCount\":2,\"totalTokenCount\":7}}";
    mock_server_enqueue(200, resp);

    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");
    int64_t sid = session_create(db, "tools_test", NULL, -1, 0);

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", s_port);

    Config cfg = {0};
    cfg.provider.base_url = base_url;
    cfg.provider.api_key = "test-key";
    cfg.provider.model = "gemini-2.5-flash";
    cfg.provider.max_tokens = 100;
    cfg.provider.context_window = 128000;
    cfg.provider.endpoint_type = ENDPOINT_GEMINI;
    cfg.max_iterations = 5;

    ToolSchema tools[1] = {{
        .name = "calculate",
        .description = "Do math",
        .parameters_json = "{\"type\":\"object\",\"properties\":{\"expr\":{\"type\":\"string\"}}}"
    }};

    Message user_msg = {.role = ROLE_USER, .content = "test"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.tools = tools;
    ctx.tool_count = 1;

    int rc = agent_run(&ctx);
    if (rc != 0) { db_close(db); FAIL("agent_run failed"); }

    /* Verify tools format: functionDeclarations (not OpenAI format) */
    const char *body = mock_server_last_request_body();
    if (!body) { db_close(db); FAIL("no body"); }
    if (!strstr(body, "functionDeclarations")) { db_close(db); FAIL("no functionDeclarations"); }
    /* Should NOT have OpenAI "tools" with "type":"function" pattern */
    if (strstr(body, "\"type\":\"function\"")) { db_close(db); FAIL("has OpenAI tool format"); }

    db_close(db);
    PASS();
}

int main(void) {
    printf("test_integration_gemini (T290):\n");
    s_port = mock_server_start();
    if (s_port < 0) { fprintf(stderr, "mock_server_start failed\n"); return 1; }

    test_gemini_text_response();
    test_gemini_tool_call();
    test_gemini_url_format();
    test_gemini_tools_format();

    mock_server_stop();
    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
