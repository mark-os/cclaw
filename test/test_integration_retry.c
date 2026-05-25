/* T127: integration test — retry + backoff with mock LLM server.
 * Mock returns 429 with Retry-After, then 200; verify agent retries
 * correctly and respects delay. Cites V2, T125. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
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
    (void)name; (void)arguments; (void)user_data;
    return strdup("ok");
}

static const char *success_response(void) {
    return "{\"id\":\"chatcmpl-ok\",\"choices\":[{\"message\":{\"role\":\"assistant\","
           "\"content\":\"Success after retry.\"},\"finish_reason\":\"stop\"}],"
           "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5,\"total_tokens\":15}}";
}

/* V2: 429 with Retry-After → agent retries and succeeds */
static void test_retry_429_with_retry_after(void) {
    TEST(retry_429_with_retry_after);

    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start failed");

    /* First request: 429 with Retry-After: 1 */
    const char *hdrs[] = {"Retry-After: 1", NULL};
    mock_server_enqueue_with_headers(429, "{\"error\":\"rate limited\"}", hdrs);
    /* Second request: 200 success */
    mock_server_enqueue(200, success_response());

    sqlite3 *db = db_open(":memory:");
    if (!db) { mock_server_stop(); FAIL("db_open failed"); }

    int64_t sid = session_create(db, "retry_test", NULL, -1, 0);
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
    cfg.system_prompt = "You are a test assistant.";

    Message user_msg = {.role = ROLE_USER, .content = "hello"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;
    ctx.tools = NULL;
    ctx.tool_count = 0;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = agent_run(&ctx);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    if (rc != 0) { db_close(db); mock_server_stop(); FAIL("agent_run should succeed after retry"); }

    /* Verify mock received exactly 2 requests (429 + 200) */
    if (mock_server_request_count() != 2) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected 2 requests, got %d", mock_server_request_count());
        db_close(db); mock_server_stop(); FAIL(msg);
    }

    /* Verify delay was respected (≥1s due to Retry-After: 1) */
    double elapsed = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    if (elapsed < 0.9) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected >=1s delay, got %.2fs", elapsed);
        db_close(db); mock_server_stop(); FAIL(msg);
    }

    /* Verify final response in DB */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    if (!entries || count != 2) {
        entry_branch_free(entries, count); db_close(db); mock_server_stop();
        FAIL("expected 2 entries (user + assistant)");
    }
    if (!entries[1].message.content || !strstr(entries[1].message.content, "Success after retry")) {
        entry_branch_free(entries, count); db_close(db); mock_server_stop();
        FAIL("assistant response should contain success text");
    }

    entry_branch_free(entries, count);
    db_close(db);
    mock_server_stop();
    PASS();
}

/* V2: multiple 429s → exponential backoff, then success */
static void test_retry_multiple_429(void) {
    TEST(retry_multiple_429);

    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start failed");

    /* Two 429s with Retry-After: 1, then success */
    const char *hdrs[] = {"Retry-After: 1", NULL};
    mock_server_enqueue_with_headers(429, "{\"error\":\"rate limited\"}", hdrs);
    mock_server_enqueue_with_headers(429, "{\"error\":\"rate limited\"}", hdrs);
    mock_server_enqueue(200, success_response());

    sqlite3 *db = db_open(":memory:");
    if (!db) { mock_server_stop(); FAIL("db_open failed"); }

    int64_t sid = session_create(db, "retry_multi", NULL, -1, 0);
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
    cfg.system_prompt = "You are a test assistant.";

    Message user_msg = {.role = ROLE_USER, .content = "hello"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;
    ctx.tools = NULL;
    ctx.tool_count = 0;

    int rc = agent_run(&ctx);
    if (rc != 0) { db_close(db); mock_server_stop(); FAIL("agent_run should succeed after retries"); }

    /* 3 requests total: 429, 429, 200 */
    if (mock_server_request_count() != 3) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected 3 requests, got %d", mock_server_request_count());
        db_close(db); mock_server_stop(); FAIL(msg);
    }

    db_close(db);
    mock_server_stop();
    PASS();
}

/* V2: 5xx also triggers retry */
static void test_retry_500(void) {
    TEST(retry_500);

    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start failed");

    /* 500 then success */
    mock_server_enqueue(500, "{\"error\":\"internal server error\"}");
    mock_server_enqueue(200, success_response());

    sqlite3 *db = db_open(":memory:");
    if (!db) { mock_server_stop(); FAIL("db_open failed"); }

    int64_t sid = session_create(db, "retry_500", NULL, -1, 0);
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
    cfg.system_prompt = "You are a test assistant.";

    Message user_msg = {.role = ROLE_USER, .content = "hello"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;
    ctx.tools = NULL;
    ctx.tool_count = 0;

    int rc = agent_run(&ctx);
    if (rc != 0) { db_close(db); mock_server_stop(); FAIL("agent_run should succeed after 500 retry"); }

    if (mock_server_request_count() != 2) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected 2 requests, got %d", mock_server_request_count());
        db_close(db); mock_server_stop(); FAIL(msg);
    }

    db_close(db);
    mock_server_stop();
    PASS();
}

/* V2/V32: all retries exhausted → agent returns error */
static void test_retry_exhausted(void) {
    TEST(retry_exhausted);

    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start failed");

    /* Enqueue enough 429s to exhaust all retries (MAX_RETRIES=5 per attempt × MAX_LLM_RETRIES=3) */
    const char *hdrs[] = {"Retry-After: 1", NULL};
    for (int i = 0; i < 20; i++)
        mock_server_enqueue_with_headers(429, "{\"error\":\"rate limited\"}", hdrs);

    sqlite3 *db = db_open(":memory:");
    if (!db) { mock_server_stop(); FAIL("db_open failed"); }

    int64_t sid = session_create(db, "retry_exhaust", NULL, -1, 0);
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
    cfg.system_prompt = "You are a test assistant.";

    Message user_msg = {.role = ROLE_USER, .content = "hello"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;
    ctx.tools = NULL;
    ctx.tool_count = 0;

    int rc = agent_run(&ctx);
    /* Should fail — all retries exhausted */
    if (rc == 0) { db_close(db); mock_server_stop(); FAIL("agent_run should fail when retries exhausted"); }

    /* Verify error entry written to DB */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    if (!entries || count < 2) {
        entry_branch_free(entries, count); db_close(db); mock_server_stop();
        FAIL("expected at least 2 entries (user + error)");
    }
    /* Last entry should be error assistant message */
    Entry *last = &entries[count - 1];
    if (last->message.role != ROLE_ASSISTANT || last->message.stop_reason != STOP_REASON_ERROR) {
        entry_branch_free(entries, count); db_close(db); mock_server_stop();
        FAIL("last entry should be assistant with stop_reason=error");
    }

    entry_branch_free(entries, count);
    db_close(db);
    mock_server_stop();
    PASS();
}

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    printf("--- test_integration_retry (T127) ---\n");
    test_retry_429_with_retry_after();
    test_retry_multiple_429();
    test_retry_500();
    test_retry_exhausted();
    printf("%d/%d passed\n", tests_passed, tests_run);
    curl_global_cleanup();
    return tests_passed == tests_run ? 0 : 1;
}
