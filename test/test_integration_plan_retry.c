/* T182: integration test — plan-only retry.
 * Mock returns bullet-list plan w/ no tool calls; verify agent re-prompts once
 * w/ act-now instruction; second mock response has tool call; verify normal
 * flow resumes. Cites: V45. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    (void)user_data; (void)arguments; (void)name;
    return strdup("done");
}

/* V45: plan-only → re-prompt → tool call → tool result → final */
static void test_plan_retry_then_tool(void) {
    TEST(plan_retry_then_tool);

    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start failed");

    /* Response 1: plan-only (bullets + promise, no tool calls) */
    mock_server_enqueue(200,
        "{\"id\":\"c1\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"Here's my plan:\\n- Read the file\\n- Fix the bug\\n- Run tests\"},"
        "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":10,"
        "\"completion_tokens\":20,\"total_tokens\":30}}");

    /* Response 2: after re-prompt, LLM issues a tool call */
    mock_server_enqueue(200,
        "{\"id\":\"c2\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":null,\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\","
        "\"function\":{\"name\":\"file_read\",\"arguments\":\"{\\\"path\\\":\\\"bug.c\\\"}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}],\"usage\":{\"prompt_tokens\":30,"
        "\"completion_tokens\":10,\"total_tokens\":40}}");

    /* Response 3: final answer after tool result */
    mock_server_enqueue(200,
        "{\"id\":\"c3\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"Fixed the bug.\"},"
        "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":40,"
        "\"completion_tokens\":5,\"total_tokens\":45}}");

    sqlite3 *db = db_open(":memory:");
    if (!db) { mock_server_stop(); FAIL("db_open failed"); }

    int64_t sid = session_create(db, "plan_retry_test", NULL, -1, 0);
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
        .name = "file_read",
        .description = "Read a file",
        .parameters_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}"
    }};

    Message user_msg = {.role = ROLE_USER, .content = "Fix the bug in bug.c"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;
    ctx.tools = tools;
    ctx.tool_count = 1;

    int rc = agent_run(&ctx);
    if (rc != 0) { db_close(db); mock_server_stop(); FAIL("agent_run failed"); }

    /* Expected entries:
     * 0: user "Fix the bug..."
     * 1: assistant plan-only (stored before retry)
     * 2: user re-prompt (V45 act-now instruction)
     * 3: assistant tool_call (file_read)
     * 4: tool result
     * 5: assistant final "Fixed the bug." */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    if (!entries || count != 6) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected 6 entries, got %d", count);
        entry_branch_free(entries, count);
        db_close(db); mock_server_stop(); FAIL(msg);
    }

    /* Entry 1: plan-only assistant */
    if (entries[1].message.role != ROLE_ASSISTANT ||
        !strstr(entries[1].message.content, "plan")) {
        entry_branch_free(entries, count); db_close(db); mock_server_stop();
        FAIL("entry[1] should be plan-only assistant");
    }

    /* Entry 2: re-prompt user message with act-now instruction */
    if (entries[2].message.role != ROLE_USER ||
        !strstr(entries[2].message.content, "Act now")) {
        entry_branch_free(entries, count); db_close(db); mock_server_stop();
        FAIL("entry[2] should be re-prompt with 'Act now'");
    }

    /* Entry 3: assistant with tool call */
    if (entries[3].message.role != ROLE_ASSISTANT ||
        entries[3].message.tool_call_count != 1) {
        entry_branch_free(entries, count); db_close(db); mock_server_stop();
        FAIL("entry[3] should be assistant with tool_call");
    }

    /* Entry 4: tool result */
    if (entries[4].message.role != ROLE_TOOL) {
        entry_branch_free(entries, count); db_close(db); mock_server_stop();
        FAIL("entry[4] should be tool result");
    }

    /* Entry 5: final assistant */
    if (entries[5].message.role != ROLE_ASSISTANT ||
        !strstr(entries[5].message.content, "Fixed")) {
        entry_branch_free(entries, count); db_close(db); mock_server_stop();
        FAIL("entry[5] should be final assistant");
    }

    /* Verify 3 LLM requests total */
    if (mock_server_request_count() != 3) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected 3 requests, got %d", mock_server_request_count());
        entry_branch_free(entries, count); db_close(db); mock_server_stop(); FAIL(msg);
    }

    entry_branch_free(entries, count);
    db_close(db);
    mock_server_stop();
    PASS();
}

/* V45: plan-only retry fires only once (max 1 retry) */
static void test_plan_retry_max_once(void) {
    TEST(plan_retry_max_once);

    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start failed");

    /* Response 1: plan-only */
    mock_server_enqueue(200,
        "{\"id\":\"c1\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"I'll do this:\\n- Step 1\\n- Step 2\"},"
        "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":10,"
        "\"completion_tokens\":10,\"total_tokens\":20}}");

    /* Response 2: still plan-only (agent should NOT retry again) */
    mock_server_enqueue(200,
        "{\"id\":\"c2\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"Let me try again:\\n- First thing\\n- Second thing\"},"
        "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":20,"
        "\"completion_tokens\":10,\"total_tokens\":30}}");

    sqlite3 *db = db_open(":memory:");
    if (!db) { mock_server_stop(); FAIL("db_open failed"); }

    int64_t sid = session_create(db, "plan_max_once", NULL, -1, 0);
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
        .name = "file_read",
        .description = "Read a file",
        .parameters_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}"
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
    /* Should succeed — second plan-only is accepted as final */
    if (rc != 0) { db_close(db); mock_server_stop(); FAIL("agent_run should succeed"); }

    /* Expected: user → plan-only asst → re-prompt → second plan-only asst (accepted) = 4 */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    if (!entries || count != 4) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected 4 entries, got %d", count);
        entry_branch_free(entries, count);
        db_close(db); mock_server_stop(); FAIL(msg);
    }

    /* Only 2 LLM requests (no third retry) */
    if (mock_server_request_count() != 2) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected 2 requests, got %d", mock_server_request_count());
        entry_branch_free(entries, count); db_close(db); mock_server_stop(); FAIL(msg);
    }

    entry_branch_free(entries, count);
    db_close(db);
    mock_server_stop();
    PASS();
}

/* V45: non-plan response (no bullets) should NOT trigger retry */
static void test_no_retry_for_normal_response(void) {
    TEST(no_retry_for_normal_response);

    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start failed");

    /* Single final response — not a plan */
    mock_server_enqueue(200,
        "{\"id\":\"c1\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"The answer is 42.\"},"
        "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":10,"
        "\"completion_tokens\":5,\"total_tokens\":15}}");

    sqlite3 *db = db_open(":memory:");
    if (!db) { mock_server_stop(); FAIL("db_open failed"); }

    int64_t sid = session_create(db, "no_retry_test", NULL, -1, 0);
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

    Message user_msg = {.role = ROLE_USER, .content = "What is the answer?"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;
    ctx.tools = NULL;
    ctx.tool_count = 0;

    int rc = agent_run(&ctx);
    if (rc != 0) { db_close(db); mock_server_stop(); FAIL("agent_run failed"); }

    /* Expected: user → assistant final = 2 entries, 1 LLM request */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    if (!entries || count != 2) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected 2 entries, got %d", count);
        entry_branch_free(entries, count);
        db_close(db); mock_server_stop(); FAIL(msg);
    }

    if (mock_server_request_count() != 1) {
        entry_branch_free(entries, count); db_close(db); mock_server_stop();
        FAIL("expected 1 request (no retry)");
    }

    entry_branch_free(entries, count);
    db_close(db);
    mock_server_stop();
    PASS();
}

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    printf("--- test_integration_plan_retry (T182) ---\n");
    test_plan_retry_then_tool();
    test_plan_retry_max_once();
    test_no_retry_for_normal_response();
    printf("%d/%d passed\n", tests_passed, tests_run);
    curl_global_cleanup();
    return tests_passed == tests_run ? 0 : 1;
}
