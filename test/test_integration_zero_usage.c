/* T231/V94: integration test — zero-usage retry (E1).
 * Mock returns HTTP 200 + 0 tokens + null content + finish_reason "stop".
 * Verify agent retries 2x primary, then 1x fallback, then writes error on exhaust. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "agent.h"
#include "db.h"
#include "config.h"
#include "mock_server.h"

static int s_port;
static int tests_run = 0, tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

/* E1 response: HTTP 200, 0 tokens, null content, finish_reason "stop" */
static const char *E1_RESP =
    "{\"id\":\"x\",\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":0,\"completion_tokens\":0,\"total_tokens\":0}}";

static const char *OK_RESP =
    "{\"id\":\"x\",\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"hello\"},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5,\"total_tokens\":15}}";

static char *mock_dispatch(const char *name, const char *args, void *ud) {
    (void)name; (void)args; (void)ud;
    return strdup("ok");
}

static sqlite3 *setup_db(int64_t *sid) {
    sqlite3 *db = db_open(":memory:");
    if (!db) return NULL;
    *sid = session_create(db, "default", NULL, -1, 0);
    Message msg = {.role = ROLE_USER, .content = "hi"};
    entry_append(db, *sid, &msg);
    return db;
}

/* E1 2x then success on 3rd retry → agent succeeds */
static void test_e1_retry_then_success(void) {
    TEST(e1_retry_then_success);
    mock_server_reset();

    int64_t sid;
    sqlite3 *db = setup_db(&sid);
    if (!db) FAIL("db_open");

    /* 2x E1, then normal response */
    mock_server_enqueue(200, E1_RESP);
    mock_server_enqueue(200, E1_RESP);
    mock_server_enqueue(200, OK_RESP);

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", s_port);

    Config cfg = {0};
    cfg.provider.model = "test";
    cfg.provider.base_url = url;
    cfg.provider.api_key = "fake";
    cfg.provider.context_window = 100000;
    cfg.provider.max_tokens = 4096;
    cfg.max_iterations = 1;

    AgentContext ctx = {.db = db, .session_id = sid, .cfg = &cfg,
                        .dispatch = mock_dispatch};
    int rc = agent_run(&ctx);
    if (rc != 0) { db_close(db); FAIL("agent_run should succeed"); }

    /* Should have made 3 requests total */
    if (mock_server_request_count() != 3) { db_close(db); FAIL("expected 3 requests"); }

    db_close(db);
    PASS();
}

/* E1 exhausted (no fallback) → error entry written */
static void test_e1_exhausted_no_fallback(void) {
    TEST(e1_exhausted_no_fallback);
    mock_server_reset();

    int64_t sid;
    sqlite3 *db = setup_db(&sid);
    if (!db) FAIL("db_open");

    /* 3x E1, no fallback configured */
    mock_server_enqueue(200, E1_RESP);
    mock_server_enqueue(200, E1_RESP);
    mock_server_enqueue(200, E1_RESP);

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", s_port);

    Config cfg = {0};
    cfg.provider.model = "test";
    cfg.provider.base_url = url;
    cfg.provider.api_key = "fake";
    cfg.provider.context_window = 100000;
    cfg.provider.max_tokens = 4096;
    cfg.max_iterations = 1;
    cfg.fallback_count = 0;

    AgentContext ctx = {.db = db, .session_id = sid, .cfg = &cfg,
                        .dispatch = mock_dispatch};
    int rc = agent_run(&ctx);
    if (rc != -1) { db_close(db); FAIL("agent_run should fail"); }

    /* Verify error entry contains provider glitch message */
    const char *resp = get_response_text(db, sid);
    if (!resp || !strstr(resp, "provider glitch")) {
        db_close(db);
        FAIL("expected provider glitch error message");
    }

    db_close(db);
    PASS();
}

/* E1 2x primary + fallback succeeds → agent succeeds */
static void test_e1_fallback_success(void) {
    TEST(e1_fallback_success);
    mock_server_reset();

    int64_t sid;
    sqlite3 *db = setup_db(&sid);
    if (!db) FAIL("db_open");

    /* 2x E1 on primary, then fallback gets OK */
    mock_server_enqueue(200, E1_RESP);
    mock_server_enqueue(200, E1_RESP);
    mock_server_enqueue(200, OK_RESP); /* 3rd attempt uses fallback, succeeds */

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", s_port);

    ProviderConfig fallback = {0};
    fallback.model = "fallback-model";
    fallback.base_url = url;
    fallback.api_key = "fake-fb";

    Config cfg = {0};
    cfg.provider.model = "test";
    cfg.provider.base_url = url;
    cfg.provider.api_key = "fake";
    cfg.provider.context_window = 100000;
    cfg.provider.max_tokens = 4096;
    cfg.max_iterations = 1;
    cfg.fallback_providers = &fallback;
    cfg.fallback_count = 1;

    AgentContext ctx = {.db = db, .session_id = sid, .cfg = &cfg,
                        .dispatch = mock_dispatch};
    int rc = agent_run(&ctx);
    if (rc != 0) { db_close(db); FAIL("agent_run should succeed with fallback"); }

    db_close(db);
    PASS();
}

int main(void) {
    s_port = mock_server_start();
    printf("test_integration_zero_usage (T231/V94):\n");
    test_e1_retry_then_success();
    test_e1_exhausted_no_fallback();
    test_e1_fallback_success();
    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    mock_server_stop();
    return tests_passed == tests_run ? 0 : 1;
}
