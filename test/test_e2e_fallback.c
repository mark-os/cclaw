#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "agent.h"
#include "db.h"
#include "config.h"
#include "llm.h"
#include "http.h"
#include <curl/curl.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)
#define SKIP(msg) do { tests_passed++; printf("SKIP: %s\n", msg); return; } while(0)

static char *mock_dispatch(const char *name, const char *arguments, void *user_data) {
    (void)name; (void)arguments; (void)user_data;
    return strdup("tool not needed");
}

/* T54: primary provider unreachable, fallback (Gemini) fires and returns valid response */
static void test_provider_fallback_live(void) {
    TEST(provider_fallback_live);
    const char *key = getenv("GEMINI_API_KEY");
    if (!key) SKIP("GEMINI_API_KEY not set");

    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open failed");

    int64_t sid = session_create(db, "fallback_test", NULL, -1, 0);
    if (sid < 0) { db_close(db); FAIL("session_create failed"); }

    /* Primary: unreachable (connection refused → -1 after retries) */
    ProviderConfig fallbacks[1] = {{
        .base_url = "https://generativelanguage.googleapis.com/v1beta/openai",
        .api_key = (char *)key,
        .model = "gemma-4-31b-it",
        .max_tokens = 64,
        .context_window = 128000,
    }};

    Config cfg = {0};
    cfg.provider.base_url = "http://127.0.0.1:1/v1"; /* unreachable */
    cfg.provider.api_key = "fake";
    cfg.provider.model = "fake-model";
    cfg.provider.max_tokens = 64;
    cfg.provider.context_window = 128000;
    cfg.max_iterations = 3;
    cfg.fallback_providers = fallbacks;
    cfg.fallback_count = 1;
    cfg.system_prompt = "Reply with exactly: fallback_ok";

    Message user_msg = {.role = ROLE_USER, .content = "Say fallback_ok"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;

    int rc = agent_run(&ctx);
    if (rc != 0) { db_close(db); FAIL("agent_run failed — fallback did not fire"); }

    /* Verify final assistant response exists */
    int entry_count = 0;
    Entry *entries = session_get_branch(db, sid, &entry_count);
    if (!entries || entry_count < 2) {
        entry_branch_free(entries, entry_count);
        db_close(db);
        FAIL("expected at least 2 entries (user + assistant)");
    }

    Entry *last = &entries[entry_count - 1];
    if (last->message.role != ROLE_ASSISTANT || !last->message.content ||
        strlen(last->message.content) == 0) {
        entry_branch_free(entries, entry_count);
        db_close(db);
        FAIL("last entry should be assistant with content");
    }

    printf("(fallback response: %.40s) ", last->message.content);
    entry_branch_free(entries, entry_count);
    db_close(db);
    PASS();
}

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    printf("--- test_integration_fallback ---\n");
    test_provider_fallback_live();
    printf("%d/%d passed\n", tests_passed, tests_run);
    curl_global_cleanup();
    return tests_passed == tests_run ? 0 : 1;
}
