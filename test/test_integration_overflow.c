/* T128: integration test — context overflow recovery.
 * Mock returns 400 with "context window" error; verify agent detects overflow
 * and returns -2. Cites T125. */
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
    (void)name; (void)arguments; (void)user_data;
    return strdup("ok");
}

/* Agent detects "context_length_exceeded" error and returns -2 */
static void test_overflow_context_length_exceeded(void) {
    TEST(overflow_context_length_exceeded);

    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start failed");

    mock_server_enqueue(400,
        "{\"error\":{\"message\":\"This model's maximum context length is 128000 tokens. "
        "However, your messages resulted in 130000 tokens. Please reduce the length.\","
        "\"type\":\"context_length_exceeded\",\"code\":\"context_length_exceeded\"}}");

    sqlite3 *db = db_open(":memory:");
    if (!db) { mock_server_stop(); FAIL("db_open failed"); }

    int64_t sid = session_create(db, "overflow_test", NULL, -1, 0);
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

    int rc = agent_run(&ctx);
    if (rc != -2) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected rc=-2 (overflow), got %d", rc);
        db_close(db); mock_server_stop(); FAIL(msg);
    }

    db_close(db);
    mock_server_stop();
    PASS();
}

/* Agent detects "maximum context length" in error body */
static void test_overflow_maximum_context_length(void) {
    TEST(overflow_maximum_context_length);

    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start failed");

    mock_server_enqueue(400,
        "{\"error\":{\"message\":\"maximum context length exceeded\"}}");

    sqlite3 *db = db_open(":memory:");
    if (!db) { mock_server_stop(); FAIL("db_open failed"); }

    int64_t sid = session_create(db, "overflow_test2", NULL, -1, 0);
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

    int rc = agent_run(&ctx);
    if (rc != -2) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected rc=-2 (overflow), got %d", rc);
        db_close(db); mock_server_stop(); FAIL(msg);
    }

    db_close(db);
    mock_server_stop();
    PASS();
}

/* Agent detects "too many tokens" variant */
static void test_overflow_too_many_tokens(void) {
    TEST(overflow_too_many_tokens);

    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start failed");

    mock_server_enqueue(400,
        "{\"error\":{\"message\":\"too many tokens in request\"}}");

    sqlite3 *db = db_open(":memory:");
    if (!db) { mock_server_stop(); FAIL("db_open failed"); }

    int64_t sid = session_create(db, "overflow_test3", NULL, -1, 0);
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

    int rc = agent_run(&ctx);
    if (rc != -2) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected rc=-2 (overflow), got %d", rc);
        db_close(db); mock_server_stop(); FAIL(msg);
    }

    db_close(db);
    mock_server_stop();
    PASS();
}

/* Non-overflow 400 should NOT return -2 (should fail normally) */
static void test_non_overflow_400(void) {
    TEST(non_overflow_400);

    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start failed");

    /* 400 without context overflow keywords — should exhaust retries */
    mock_server_enqueue(400, "{\"error\":{\"message\":\"invalid request format\"}}");
    mock_server_enqueue(400, "{\"error\":{\"message\":\"invalid request format\"}}");
    mock_server_enqueue(400, "{\"error\":{\"message\":\"invalid request format\"}}");

    sqlite3 *db = db_open(":memory:");
    if (!db) { mock_server_stop(); FAIL("db_open failed"); }

    int64_t sid = session_create(db, "non_overflow", NULL, -1, 0);
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

    int rc = agent_run(&ctx);
    /* Should fail with -1 (generic error), NOT -2 (overflow) */
    if (rc == -2) {
        db_close(db); mock_server_stop();
        FAIL("non-overflow 400 should not return -2");
    }
    if (rc == 0) {
        db_close(db); mock_server_stop();
        FAIL("should not succeed with 400 error");
    }

    db_close(db);
    mock_server_stop();
    PASS();
}

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    printf("--- test_integration_overflow (T128) ---\n");
    test_overflow_context_length_exceeded();
    test_overflow_maximum_context_length();
    test_overflow_too_many_tokens();
    test_non_overflow_400();
    printf("%d/%d passed\n", tests_passed, tests_run);
    curl_global_cleanup();
    return tests_passed == tests_run ? 0 : 1;
}
