/* Test V126 (fallback resp free) and V127 (5xx outer break).
 * V126: verify fallback works without leak (functional correctness).
 * V127: verify persistent 503 produces at most MAX_RETRIES (5) requests
 *        from inner loop; outer loop must break, not continue.
 *
 * NOTE: inner retry sleeps with exponential backoff (1s,2s,4s,...).
 * To keep test fast, we enqueue only 2 failures (inner retries 2×, sleeps ~3s)
 * then success. For V127, we test the break behavior indirectly. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static const char *ok_resp(void) {
    return "{\"id\":\"x\",\"choices\":[{\"message\":{\"role\":\"assistant\","
           "\"content\":\"done\"},\"finish_reason\":\"stop\"}],"
           "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5,\"total_tokens\":15}}";
}

/* V126: primary fails (502×2), fallback succeeds — verify no crash */
static void test_V126_fallback_resp_free(void) {
    TEST(V126_fallback_resp_free);
    mock_server_reset();

    /* Primary: 2 × 502 then would retry more, but we also enqueue the fallback success.
     * Inner loop retries 2× (sleep 1+2=3s), then gives up on 3rd 502.
     * Actually inner loop retries up to MAX_RETRIES(5). Let's enqueue 5 502s + 1 fallback. */
    const char *hdrs[] = {"Retry-After: 1", NULL};
    for (int i = 0; i < 5; i++)
        mock_server_enqueue_with_headers(502, "{\"error\":\"bad gateway\"}", hdrs);
    /* Fallback hits same server — 200 success */
    mock_server_enqueue(200, ok_resp());

    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");

    int64_t sid = session_create(db, "v126", NULL, -1, 0);
    if (sid < 0) { db_close(db); FAIL("session_create"); }

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", s_port);

    Config cfg = {0};
    cfg.provider.base_url = base_url;
    cfg.provider.api_key = "pk";
    cfg.provider.model = "m";
    cfg.provider.max_tokens = 256;
    cfg.provider.context_window = 128000;
    cfg.max_iterations = 5;
    cfg.system_prompt = "test";

    ProviderConfig fb = {0};
    fb.base_url = base_url;
    fb.api_key = "fk";
    fb.model = "fb-model";
    fb.max_tokens = 256;
    fb.context_window = 128000;
    cfg.fallback_providers = &fb;
    cfg.fallback_count = 1;

    Message user_msg = {.role = ROLE_USER, .content = "hi"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;

    int rc = agent_run(&ctx);
    if (rc != 0) { db_close(db); FAIL("agent_run should succeed via fallback"); }

    /* 5 primary + 1 fallback = 6 */
    int rcount = mock_server_request_count();
    if (rcount != 6) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected 6 requests, got %d", rcount);
        db_close(db); FAIL(msg);
    }

    db_close(db);
    PASS();
}

/* V127: persistent 503 (no fallback) — outer loop must break (not re-retry).
 * With V127 fix: inner loop does 5 retries, returns 503. Outer loop sees 5xx → break.
 * Total requests = 5 (inner MAX_RETRIES only). Without fix: would be 15 (3×5). */
static void test_V127_5xx_outer_break(void) {
    TEST(V127_5xx_outer_break);
    mock_server_reset();

    /* Enqueue 20 × 503 — only 5 should be consumed if V127 works */
    const char *hdrs[] = {"Retry-After: 1", NULL};
    for (int i = 0; i < 20; i++)
        mock_server_enqueue_with_headers(503, "{\"error\":\"unavailable\"}", hdrs);

    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");

    int64_t sid = session_create(db, "v127", NULL, -1, 0);
    if (sid < 0) { db_close(db); FAIL("session_create"); }

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", s_port);

    Config cfg = {0};
    cfg.provider.base_url = base_url;
    cfg.provider.api_key = "pk";
    cfg.provider.model = "m";
    cfg.provider.max_tokens = 256;
    cfg.provider.context_window = 128000;
    cfg.max_iterations = 5;
    cfg.system_prompt = "test";
    cfg.fallback_count = 0;

    Message user_msg = {.role = ROLE_USER, .content = "hi"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;

    int rc = agent_run(&ctx);
    if (rc == 0) { db_close(db); FAIL("agent_run should fail on persistent 503"); }

    /* V127: exactly 5 requests (inner only). If >5, outer loop re-retried. */
    int rcount = mock_server_request_count();
    if (rcount > 5) {
        char msg[80];
        snprintf(msg, sizeof(msg),
                 "expected <=5 (inner only), got %d (outer re-retried)", rcount);
        db_close(db); FAIL(msg);
    }

    db_close(db);
    PASS();
}

int main(void) {
    printf("test_integration_fallback_fixes (V126, V127):\n");

    s_port = mock_server_start();
    if (s_port <= 0) {
        fprintf(stderr, "FAIL: could not start mock server\n");
        return 1;
    }

    test_V126_fallback_resp_free();
    test_V127_5xx_outer_break();

    mock_server_stop();

    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}

