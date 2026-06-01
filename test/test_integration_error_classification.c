/* T236: integration test — error classification E1-E12.
 * Verify each failure mode produces correct stop_reason + user message. */
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

static const char *get_last_assistant(sqlite3 *db, int64_t sid) {
    return get_response_text(db, sid);
}

/* E11: 401 auth failure → specific error message */
static void test_e11_auth_failure(void) {
    TEST(e11_auth_failure);
    mock_server_reset();

    int64_t sid;
    sqlite3 *db = setup_db(&sid);
    if (!db) FAIL("db_open");

    mock_server_enqueue(401, "{\"error\":{\"message\":\"invalid api key\"}}");

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", s_port);

    Config cfg = {0};
    cfg.provider.model = "test";
    cfg.provider.base_url = url;
    cfg.provider.api_key = "bad-key";
    cfg.provider.context_window = 100000;
    cfg.provider.max_tokens = 4096;
    cfg.max_iterations = 1;

    AgentContext ctx = {.db = db, .session_id = sid, .cfg = &cfg,
                        .dispatch = mock_dispatch};
    agent_run(&ctx);

    const char *resp = get_last_assistant(db, sid);
    if (!resp || !strstr(resp, "authentication failed")) {
        db_close(db);
        FAIL("expected auth failure message");
    }
    db_close(db);
    PASS();
}

/* E12: 404 model not found */
static void test_e12_model_not_found(void) {
    TEST(e12_model_not_found);
    mock_server_reset();

    int64_t sid;
    sqlite3 *db = setup_db(&sid);
    if (!db) FAIL("db_open");

    mock_server_enqueue(404, "{\"error\":{\"message\":\"model not found\"}}");

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", s_port);

    Config cfg = {0};
    cfg.provider.model = "nonexistent";
    cfg.provider.base_url = url;
    cfg.provider.api_key = "key";
    cfg.provider.context_window = 100000;
    cfg.provider.max_tokens = 4096;
    cfg.max_iterations = 1;

    AgentContext ctx = {.db = db, .session_id = sid, .cfg = &cfg,
                        .dispatch = mock_dispatch};
    agent_run(&ctx);

    const char *resp = get_last_assistant(db, sid);
    if (!resp || !strstr(resp, "model not available")) {
        db_close(db);
        FAIL("expected model not available message");
    }
    db_close(db);
    PASS();
}

/* E8: finish_reason=length → partial content stored with STOP_REASON_LENGTH */
static void test_e8_length(void) {
    TEST(e8_length);
    mock_server_reset();

    int64_t sid;
    sqlite3 *db = setup_db(&sid);
    if (!db) FAIL("db_open");

    mock_server_enqueue(200,
        "{\"id\":\"x\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"partial output here\"},\"finish_reason\":\"length\"}],"
        "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":100,\"total_tokens\":110}}");

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", s_port);

    Config cfg = {0};
    cfg.provider.model = "test";
    cfg.provider.base_url = url;
    cfg.provider.api_key = "key";
    cfg.provider.context_window = 100000;
    cfg.provider.max_tokens = 4096;
    cfg.max_iterations = 1;

    AgentContext ctx = {.db = db, .session_id = sid, .cfg = &cfg,
                        .dispatch = mock_dispatch};
    int rc = agent_run(&ctx);

    /* Should succeed (partial content is still a valid response) */
    if (rc != 0) { db_close(db); FAIL("agent_run should return 0 for length"); }

    const char *resp = get_last_assistant(db, sid);
    if (!resp || !strstr(resp, "partial output here")) {
        db_close(db);
        FAIL("expected partial content preserved");
    }
    db_close(db);
    PASS();
}

/* E9: content_filter → error entry */
static void test_e9_content_filter(void) {
    TEST(e9_content_filter);
    mock_server_reset();

    int64_t sid;
    sqlite3 *db = setup_db(&sid);
    if (!db) FAIL("db_open");

    mock_server_enqueue(200,
        "{\"id\":\"x\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":null},\"finish_reason\":\"content_filter\"}],"
        "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":0,\"total_tokens\":10}}");

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", s_port);

    Config cfg = {0};
    cfg.provider.model = "test";
    cfg.provider.base_url = url;
    cfg.provider.api_key = "key";
    cfg.provider.context_window = 100000;
    cfg.provider.max_tokens = 4096;
    cfg.max_iterations = 1;

    AgentContext ctx = {.db = db, .session_id = sid, .cfg = &cfg,
                        .dispatch = mock_dispatch};
    int rc = agent_run(&ctx);

    if (rc != -1) { db_close(db); FAIL("agent_run should fail for content_filter"); }

    const char *resp = get_last_assistant(db, sid);
    if (!resp || !strstr(resp, "filtered by provider safety")) {
        db_close(db);
        FAIL("expected content filter message");
    }
    db_close(db);
    PASS();
}

/* E2: 429 exhausted → rate limit message */
static void test_e2_rate_limit_exhausted(void) {
    TEST(e2_rate_limit_exhausted);
    mock_server_reset();

    int64_t sid;
    sqlite3 *db = setup_db(&sid);
    if (!db) FAIL("db_open");

    /* Enqueue enough 429s to exhaust retries (MAX_RETRIES=5 in retry_stream × MAX_LLM_RETRIES=3) */
    for (int i = 0; i < 20; i++)
        mock_server_enqueue(429, "{\"error\":{\"message\":\"rate limited\"}}");

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", s_port);

    Config cfg = {0};
    cfg.provider.model = "test";
    cfg.provider.base_url = url;
    cfg.provider.api_key = "key";
    cfg.provider.context_window = 100000;
    cfg.provider.max_tokens = 4096;
    cfg.max_iterations = 1;

    AgentContext ctx = {.db = db, .session_id = sid, .cfg = &cfg,
                        .dispatch = mock_dispatch};
    agent_run(&ctx);

    const char *resp = get_last_assistant(db, sid);
    if (!resp || !strstr(resp, "rate limited")) {
        db_close(db);
        FAIL("expected rate limit message");
    }
    db_close(db);
    PASS();
}

int main(void) {
    alarm(120);
    s_port = mock_server_start();
    printf("test_integration_error_classification (T236):\n");
    test_e11_auth_failure();
    test_e12_model_not_found();
    test_e8_length();
    test_e9_content_filter();
    test_e2_rate_limit_exhausted();
    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    mock_server_stop();
    return tests_passed == tests_run ? 0 : 1;
}
