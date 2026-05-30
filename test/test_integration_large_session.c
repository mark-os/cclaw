/* T178: integration test — large session memory.
 * Insert 500+ entries, run agent loop w/ mock server, verify RSS stays
 * bounded (check /proc/self/status); verify context_plan truncates correctly.
 * Cites: V41, V56 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <curl/curl.h>
#include "agent.h"
#include "db.h"
#include "config.h"
#include "context.h"
#include "mock_server.h"
static int s_port;

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

/* Read VmRSS from /proc/self/status in KB */
static long get_rss_kb(void) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[256];
    long rss = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            rss = atol(line + 6);
            break;
        }
    }
    fclose(f);
    return rss;
}

static char *mock_dispatch(const char *name, const char *arguments, void *user_data) {
    (void)name; (void)arguments; (void)user_data;
    return strdup("ok");
}

/* Insert N user+assistant entry pairs into session */
static void seed_entries(sqlite3 *db, int64_t sid, int count) {
    char buf[256];
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "User message number %d with some padding text to simulate real content.", i);
        Message user_msg = {.role = ROLE_USER, .content = buf};
        entry_append(db, sid, &user_msg);

        snprintf(buf, sizeof(buf), "Assistant response number %d. Here is some helpful information about the topic.", i);
        Message asst_msg = {.role = ROLE_ASSISTANT, .content = buf, .stop_reason = STOP_REASON_STOP};
        entry_append(db, sid, &asst_msg);
    }
}

/* T178: context_plan truncates correctly with 500+ entries */
static void test_context_plan_truncation(void) {
    TEST(context_plan_truncation_500_entries);

    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open failed");

    int64_t sid = session_create(db, "large_session", NULL, -1, 0);
    if (sid < 0) { db_close(db); FAIL("session_create failed"); }

    /* Insert 500 pairs = 1000 entries */
    seed_entries(db, sid, 500);

    /* Small context window → forces aggressive truncation */
    Config cfg = {0};
    cfg.provider.context_window = 8000; /* ~8K tokens budget → 60% = 4800 */
    cfg.max_history_tokens = 0; /* use default 60% */

    ContextPlan plan;
    int rc = context_plan(db, sid, &cfg, &plan);
    if (rc != 0) { db_close(db); FAIL("context_plan failed"); }

    /* Verify truncation happened: cut > 0 means some entries excluded */
    if (plan.cut <= 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected cut > 0, got %d (count=%d)", plan.cut, plan.count);
        context_plan_free(&plan);
        db_close(db);
        FAIL(msg);
    }

    /* Verify included entries fit within budget */
    int included_tokens = 0;
    for (int i = plan.cut; i < plan.count; i++)
        included_tokens += plan.entries[i].token_estimate;
    if (included_tokens > plan.budget) {
        char msg[128];
        snprintf(msg, sizeof(msg), "included tokens %d > budget %d", included_tokens, plan.budget);
        context_plan_free(&plan);
        db_close(db);
        FAIL(msg);
    }

    /* V56: verify plan entries only have lightweight metadata (no content loaded) */
    /* This is structural — PlanEntry struct has no content field by design */
    if (plan.count < 900) {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected ~1000 entries after V28 filter, got %d", plan.count);
        context_plan_free(&plan);
        db_close(db);
        FAIL(msg);
    }

    context_plan_free(&plan);
    db_close(db);
    PASS();
}

/* T178: agent loop with 500+ entries — RSS stays bounded */
static void test_large_session_rss(void) {
    TEST(large_session_rss_bounded);

    mock_server_reset();

    /* Mock returns a simple final response */
    mock_server_enqueue(200,
        "{\"id\":\"chatcmpl-mock\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"Done.\"},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":100,"
        "\"completion_tokens\":5,\"total_tokens\":105}}");

    sqlite3 *db = db_open(":memory:");
    if (!db) { FAIL("db_open failed"); }

    int64_t sid = session_create(db, "rss_test", NULL, -1, 0);
    if (sid < 0) { db_close(db); FAIL("session_create failed"); }

    /* Seed 500 pairs = 1000 entries */
    seed_entries(db, sid, 500);

    /* Add final user message to trigger agent */
    Message user_msg = {.role = ROLE_USER, .content = "What is 2+2?"};
    entry_append(db, sid, &user_msg);

    long rss_before = get_rss_kb();

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", s_port);

    Config cfg = {0};
    cfg.provider.base_url = base_url;
    cfg.provider.api_key = "mock-key";
    cfg.provider.model = "mock-model";
    cfg.provider.max_tokens = 256;
    cfg.provider.context_window = 128000;
    cfg.max_iterations = 5;
    cfg.system_prompt = "You are helpful.";

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;
    ctx.tools = NULL;
    ctx.tool_count = 0;

    int rc = agent_run(&ctx);
    if (rc != 0) { db_close(db); FAIL("agent_run failed"); }

    long rss_after = get_rss_kb();

    /* V41: RSS should not grow proportionally to session size.
     * 1000 entries × ~100 bytes content = ~100KB of content.
     * With streaming (V41), we should NOT load all content into memory.
     * Allow generous 50MB growth cap (accounts for SQLite mmap, curl buffers). */
    long rss_growth = rss_after - rss_before;
    if (rss_growth > 50 * 1024) { /* 50MB */
        char msg[128];
        snprintf(msg, sizeof(msg), "RSS grew %ld KB (before=%ld, after=%ld) — exceeds 50MB cap",
                 rss_growth, rss_before, rss_after);
        db_close(db);
       
        FAIL(msg);
    }

    /* Verify agent produced a response */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    if (!entries || count < 1002) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected >=1002 entries, got %d", count);
        entry_branch_free(entries, count);
        db_close(db); FAIL(msg);
    }
    /* Last entry should be assistant final */
    if (entries[count - 1].message.role != ROLE_ASSISTANT) {
        entry_branch_free(entries, count);
        db_close(db); FAIL("last entry should be assistant");
    }
    entry_branch_free(entries, count);

    db_close(db);
    PASS();
}

/* T178: context_plan with large session uses only integer columns (V56) */
static void test_context_plan_no_content_load(void) {
    TEST(context_plan_no_content_load);

    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open failed");

    int64_t sid = session_create(db, "plan_test", NULL, -1, 0);
    if (sid < 0) { db_close(db); FAIL("session_create failed"); }

    /* Insert 250 pairs with large content to make overflow pages likely */
    char big_content[2048];
    memset(big_content, 'x', sizeof(big_content) - 1);
    big_content[sizeof(big_content) - 1] = '\0';

    for (int i = 0; i < 250; i++) {
        Message user_msg = {.role = ROLE_USER, .content = big_content};
        entry_append(db, sid, &user_msg);
        Message asst_msg = {.role = ROLE_ASSISTANT, .content = big_content, .stop_reason = STOP_REASON_STOP};
        entry_append(db, sid, &asst_msg);
    }

    long rss_before = get_rss_kb();

    Config cfg = {0};
    cfg.provider.context_window = 16000;

    ContextPlan plan;
    int rc = context_plan(db, sid, &cfg, &plan);
    if (rc != 0) { db_close(db); FAIL("context_plan failed"); }

    long rss_after = get_rss_kb();

    /* context_plan should NOT load content — RSS growth should be minimal.
     * 500 entries × 2KB content = 1MB if loaded. Allow 5MB growth (SQLite overhead). */
    long rss_growth = rss_after - rss_before;
    if (rss_growth > 5 * 1024) {
        char msg[128];
        snprintf(msg, sizeof(msg), "context_plan RSS grew %ld KB — may be loading content", rss_growth);
        context_plan_free(&plan);
        db_close(db);
        FAIL(msg);
    }

    /* Verify plan has entries and truncation */
    if (plan.count < 400) {
        context_plan_free(&plan);
        db_close(db);
        FAIL("expected 500 entries in plan");
    }
    if (plan.cut <= 0) {
        context_plan_free(&plan);
        db_close(db);
        FAIL("expected truncation with 16K window");
    }

    context_plan_free(&plan);
    db_close(db);
    PASS();
}

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    s_port = mock_server_start();
    printf("--- test_integration_large_session (T178) ---\n");
    test_context_plan_truncation();
    test_large_session_rss();
    test_context_plan_no_content_load();
    printf("%d/%d passed\n", tests_passed, tests_run);
    mock_server_stop();
    curl_global_cleanup();
    return tests_passed == tests_run ? 0 : 1;
}
