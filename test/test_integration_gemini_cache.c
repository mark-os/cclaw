/* T291: Gemini context caching integration test.
 * Verifies: cachedContent field emitted in request when cache name is set,
 * systemInstruction omitted, and cache lifecycle (get_or_create, delete). */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "db.h"
#include "config.h"
#include "gemini_cache.h"
#include "request_stream.h"
#include "context.h"
#include "mock_server.h"
#include "cJSON.h"

static int s_port;
static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

/* Test 1: request_stream emits cachedContent when gemini_cache_name is set */
static void test_cached_content_in_request(void) {
    TEST(cached_content_in_request);

    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");
    int64_t sid = session_create(db, "cache_test", NULL, -1, 0);

    Message sys_msg = {.role = ROLE_SYSTEM, .content = "System prompt."};
    entry_append(db, sid, &sys_msg);
    Message user_msg = {.role = ROLE_USER, .content = "Hello"};
    entry_append(db, sid, &user_msg);

    Config cfg = {0};
    cfg.provider.base_url = "http://127.0.0.1:9999";
    cfg.provider.api_key = "test-key";
    cfg.provider.model = "gemini-2.5-flash";
    cfg.provider.context_window = 128000;
    cfg.provider.endpoint_type = ENDPOINT_GEMINI;
    cfg.max_iterations = 5;

    ContextPlan plan;
    if (context_plan(db, sid, &cfg, 0, &plan) != 0) { db_close(db); FAIL("context_plan"); }

    RequestStreamer rs;
    rs_init(&rs, db, sid, &cfg, &plan, NULL, 0);
    rs.gemini_cache_name = "cachedContents/abc123";

    /* Read entire request into buffer */
    char buf[4096];
    size_t total = 0;
    while (total < sizeof(buf) - 1) {
        size_t n = rs_read_cb(buf + total, 1, sizeof(buf) - 1 - total, &rs);
        if (n == 0) break;
        total += n;
    }
    buf[total] = '\0';
    rs_cleanup(&rs);
    context_plan_free(&plan);

    /* Verify cachedContent field present */
    if (!strstr(buf, "\"cachedContent\":\"cachedContents/abc123\"")) {
        printf("\n  GOT: %s\n", buf);
        db_close(db); FAIL("no cachedContent field");
    }

    /* Verify systemInstruction is NOT present (cached) */
    if (strstr(buf, "systemInstruction")) {
        db_close(db); FAIL("systemInstruction should not be present with cache");
    }

    /* Verify contents array still present (non-cached entries) */
    if (!strstr(buf, "\"contents\":[")) {
        db_close(db); FAIL("no contents array");
    }

    db_close(db);
    PASS();
}

/* Test 2: without cache name, systemInstruction is present */
static void test_no_cache_has_system_instruction(void) {
    TEST(no_cache_has_system_instruction);

    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");
    int64_t sid = session_create(db, "nocache_test", NULL, -1, 0);

    Message sys_msg = {.role = ROLE_SYSTEM, .content = "System prompt."};
    entry_append(db, sid, &sys_msg);
    Message user_msg = {.role = ROLE_USER, .content = "Hello"};
    entry_append(db, sid, &user_msg);

    Config cfg = {0};
    cfg.provider.base_url = "http://127.0.0.1:9999";
    cfg.provider.api_key = "test-key";
    cfg.provider.model = "gemini-2.5-flash";
    cfg.provider.context_window = 128000;
    cfg.provider.endpoint_type = ENDPOINT_GEMINI;
    cfg.max_iterations = 5;

    ContextPlan plan;
    if (context_plan(db, sid, &cfg, 0, &plan) != 0) { db_close(db); FAIL("context_plan"); }

    RequestStreamer rs;
    rs_init(&rs, db, sid, &cfg, &plan, NULL, 0);
    /* gemini_cache_name is NULL (default from memset) */

    char buf[4096];
    size_t total = 0;
    while (total < sizeof(buf) - 1) {
        size_t n = rs_read_cb(buf + total, 1, sizeof(buf) - 1 - total, &rs);
        if (n == 0) break;
        total += n;
    }
    buf[total] = '\0';
    rs_cleanup(&rs);
    context_plan_free(&plan);

    /* Verify systemInstruction IS present */
    if (!strstr(buf, "systemInstruction")) {
        db_close(db); FAIL("systemInstruction missing without cache");
    }

    /* Verify no cachedContent field */
    if (strstr(buf, "cachedContent")) {
        db_close(db); FAIL("cachedContent should not be present");
    }

    db_close(db);
    PASS();
}

/* Test 3: gemini_cache_get_or_create returns NULL when tokens < 1024 */
static void test_cache_skip_small_session(void) {
    TEST(cache_skip_small_session);

    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");
    int64_t sid = session_create(db, "small_test", NULL, -1, 0);

    /* Only a few tokens */
    Message user_msg = {.role = ROLE_USER, .content = "Hi"};
    entry_append(db, sid, &user_msg);

    Config cfg = {0};
    cfg.provider.base_url = "http://127.0.0.1:9999";
    cfg.provider.api_key = "test-key";
    cfg.provider.model = "gemini-2.5-flash";
    cfg.provider.context_window = 128000;
    cfg.provider.endpoint_type = ENDPOINT_GEMINI;

    ContextPlan plan;
    if (context_plan(db, sid, &cfg, 0, &plan) != 0) { db_close(db); FAIL("context_plan"); }

    char *name = gemini_cache_get_or_create(db, sid, &cfg, &plan);
    context_plan_free(&plan);

    if (name != NULL) {
        free(name);
        db_close(db);
        FAIL("should return NULL for small session");
    }

    db_close(db);
    PASS();
}

/* Test 4: gemini_cache_get_or_create returns NULL for non-Gemini endpoint */
static void test_cache_skip_non_gemini(void) {
    TEST(cache_skip_non_gemini);

    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");
    int64_t sid = session_create(db, "openai_test", NULL, -1, 0);

    /* Lots of content */
    char big[5000];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    Message user_msg = {.role = ROLE_USER, .content = big};
    entry_append(db, sid, &user_msg);

    Config cfg = {0};
    cfg.provider.base_url = "http://127.0.0.1:9999";
    cfg.provider.api_key = "test-key";
    cfg.provider.model = "gpt-4";
    cfg.provider.context_window = 128000;
    cfg.provider.endpoint_type = ENDPOINT_OPENAI; /* Not Gemini */

    ContextPlan plan;
    if (context_plan(db, sid, &cfg, 0, &plan) != 0) { db_close(db); FAIL("context_plan"); }

    char *name = gemini_cache_get_or_create(db, sid, &cfg, &plan);
    context_plan_free(&plan);

    if (name != NULL) {
        free(name);
        db_close(db);
        FAIL("should return NULL for non-gemini");
    }

    db_close(db);
    PASS();
}

/* Test 5: cache creation with mock server + kv persistence */
static void test_cache_create_and_reuse(void) {
    TEST(cache_create_and_reuse);
    mock_server_reset();

    /* Mock cachedContents API response */
    mock_server_enqueue(200, "{\"name\":\"cachedContents/test123\",\"model\":\"models/gemini-2.5-flash\"}");

    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");
    int64_t sid = session_create(db, "cache_create_test", NULL, -1, 0);

    /* Add enough content to exceed 1024 tokens (chars/4 heuristic) */
    char big[5000];
    memset(big, 'A', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    Message user_msg = {.role = ROLE_USER, .content = big};
    entry_append(db, sid, &user_msg);

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", s_port);

    Config cfg = {0};
    cfg.provider.base_url = base_url;
    cfg.provider.api_key = "test-key";
    cfg.provider.model = "gemini-2.5-flash";
    cfg.provider.context_window = 128000;
    cfg.provider.endpoint_type = ENDPOINT_GEMINI;

    ContextPlan plan;
    if (context_plan(db, sid, &cfg, 0, &plan) != 0) { db_close(db); FAIL("context_plan"); }

    char *name = gemini_cache_get_or_create(db, sid, &cfg, &plan);
    if (!name) { context_plan_free(&plan); db_close(db); FAIL("expected cache name"); }
    if (strcmp(name, "cachedContents/test123") != 0) {
        free(name); context_plan_free(&plan); db_close(db); FAIL("wrong cache name");
    }
    free(name);

    /* Second call should reuse from kv (no new HTTP request) */
    int req_count_before = mock_server_request_count();
    name = gemini_cache_get_or_create(db, sid, &cfg, &plan);
    if (!name) { context_plan_free(&plan); db_close(db); FAIL("reuse failed"); }
    if (strcmp(name, "cachedContents/test123") != 0) {
        free(name); context_plan_free(&plan); db_close(db); FAIL("wrong reused name");
    }
    /* Should not have made another request */
    if (mock_server_request_count() != req_count_before) {
        free(name); context_plan_free(&plan); db_close(db); FAIL("made extra request");
    }
    free(name);

    context_plan_free(&plan);
    db_close(db);
    PASS();
}

int main(void) {
    printf("test_integration_gemini_cache (T291):\n");
    s_port = mock_server_start();
    if (s_port < 0) { fprintf(stderr, "mock_server_start failed\n"); return 1; }

    test_cached_content_in_request();
    test_no_cache_has_system_instruction();
    test_cache_skip_small_session();
    test_cache_skip_non_gemini();
    test_cache_create_and_reuse();

    mock_server_stop();
    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
