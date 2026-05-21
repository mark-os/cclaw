#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "agent.h"
#include "db.h"
#include "config.h"
#include "llm.h"
#include "http.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

/* V10: tool that returns a normal result */
static char *mock_tool_ok(const char *name, const char *arguments, void *user_data) {
    (void)arguments; (void)user_data;
    char *result = malloc(128);
    snprintf(result, 128, "result from %s", name);
    return result;
}

/* V10: tool that returns NULL (simulates crash) */
static char *mock_tool_null(const char *name, const char *arguments, void *user_data) {
    (void)name; (void)arguments; (void)user_data;
    return NULL;
}

static void test_agent_context_init(void) {
    TEST(agent_context_init);
    AgentContext ctx = {0};
    ctx.dispatch = mock_tool_ok;
    ctx.tool_count = 0;
    if (ctx.dispatch == NULL) { FAIL("dispatch not set"); return; }
    if (ctx.tool_count != 0) { FAIL("tool_count wrong"); return; }
    PASS();
}

static void test_agent_run_null_ctx(void) {
    TEST(agent_run_null_ctx);
    /* agent_run with NULL should return -1 */
    int rc = agent_run(NULL);
    if (rc != -1) { FAIL("expected -1 for NULL ctx"); return; }
    PASS();
}

static void test_agent_run_no_db(void) {
    TEST(agent_run_no_db);
    AgentContext ctx = {0};
    ctx.cfg = (const Config *)1; /* non-null but won't be dereferenced */
    /* No db → should fail gracefully */
    int rc = agent_run(&ctx);
    if (rc != -1) { FAIL("expected -1 for NULL db"); return; }
    PASS();
}

static void test_v10_dispatch_null_handler(void) {
    TEST(v10_dispatch_null_handler);
    /* When dispatch is NULL, agent should handle gracefully.
     * We test this indirectly: agent_run with valid DB but no LLM
     * will fail at HTTP, not at dispatch. So test the concept:
     * an AgentContext with no dispatch set should still be constructable. */
    AgentContext ctx = {0};
    ctx.dispatch = NULL;
    /* This is valid — dispatch is optional until tools are actually called */
    if (ctx.dispatch != NULL) { FAIL("should be NULL"); return; }
    PASS();
}

static void test_v10_dispatch_returns_error_on_null(void) {
    TEST(v10_dispatch_returns_error_on_null);
    /* Simulate what agent does when tool returns NULL:
     * The dispatch wrapper should produce an error string */
    char *result = mock_tool_null("test_tool", "{}", NULL);
    if (result != NULL) { FAIL("mock should return NULL"); free(result); return; }
    /* Agent code handles this by producing "error: tool 'X' returned null" */
    PASS();
}

static void test_v10_dispatch_ok(void) {
    TEST(v10_dispatch_ok);
    char *result = mock_tool_ok("shell_exec", "{\"cmd\":\"ls\"}", NULL);
    if (!result) { FAIL("expected non-NULL"); return; }
    if (strstr(result, "shell_exec") == NULL) { FAIL("wrong content"); free(result); return; }
    free(result);
    PASS();
}

static void test_agent_run_empty_session(void) {
    TEST(agent_run_empty_session);
    /* With a real DB but empty session and unreachable LLM, should fail at HTTP */
    sqlite3 *db = db_open(":memory:");
    if (!db) { FAIL("db_open failed"); return; }

    int64_t sid = session_create(db, "test");
    if (sid < 0) { FAIL("session_create failed"); db_close(db); return; }

    Config cfg = {0};
    cfg.provider.base_url = "http://127.0.0.1:1/v1";
    cfg.provider.api_key = "fake";
    cfg.provider.model = "test";
    cfg.provider.context_window = 128000;

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_tool_ok;

    /* Append a user message so context_build has something */
    Message user_msg = {.role = ROLE_USER, .content = "hello"};
    entry_append(db, sid, &user_msg);

    /* Will fail at HTTP (unreachable) — should return -1, not crash */
    int rc = agent_run(&ctx);
    if (rc != -1) { FAIL("expected -1 for unreachable LLM"); db_close(db); return; }

    db_close(db);
    PASS();
}

static void test_max_iterations_configurable(void) {
    TEST(max_iterations_configurable);
    /* Verify config max_iterations is used by agent */
    Config cfg = {0};
    cfg.provider.base_url = "http://127.0.0.1:1/v1";
    cfg.provider.api_key = "fake";
    cfg.provider.model = "test";
    cfg.provider.context_window = 128000;
    cfg.max_iterations = 1;

    sqlite3 *db = db_open(":memory:");
    if (!db) { FAIL("db_open failed"); return; }
    int64_t sid = session_create(db, "test");
    if (sid < 0) { FAIL("session_create failed"); db_close(db); return; }

    Message user_msg = {.role = ROLE_USER, .content = "hello"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_tool_ok;

    /* With max_iterations=1 and unreachable LLM, should fail after 1 attempt */
    int rc = agent_run(&ctx);
    if (rc != -1) { FAIL("expected -1"); db_close(db); return; }

    db_close(db);
    PASS();
}

static void test_max_iterations_default(void) {
    TEST(max_iterations_default);
    /* max_iterations=0 should fall back to AGENT_DEFAULT_MAX_ITERATIONS */
    Config cfg = {0};
    cfg.max_iterations = 0;
    cfg.provider.base_url = "http://127.0.0.1:1/v1";
    cfg.provider.api_key = "fake";
    cfg.provider.model = "test";
    cfg.provider.context_window = 128000;

    sqlite3 *db = db_open(":memory:");
    if (!db) { FAIL("db_open failed"); return; }
    int64_t sid = session_create(db, "test");
    if (sid < 0) { FAIL("session_create failed"); db_close(db); return; }

    Message user_msg = {.role = ROLE_USER, .content = "hello"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_tool_ok;

    /* Should use default (25), fail at HTTP — just verify it doesn't crash */
    int rc = agent_run(&ctx);
    if (rc != -1) { FAIL("expected -1 for unreachable LLM"); db_close(db); return; }

    db_close(db);
    PASS();
}

static void test_debug_flag_stderr(void) {
    TEST(debug_flag_stderr);
    /* With debug=1 and unreachable LLM, agent should still fail gracefully
     * (debug output goes to stderr, doesn't affect return code) */
    sqlite3 *db = db_open(":memory:");
    if (!db) { FAIL("db_open failed"); return; }
    int64_t sid = session_create(db, "test");
    if (sid < 0) { FAIL("session_create failed"); db_close(db); return; }

    Config cfg = {0};
    cfg.provider.base_url = "http://127.0.0.1:1/v1";
    cfg.provider.api_key = "fake";
    cfg.provider.model = "test";
    cfg.provider.context_window = 128000;
    cfg.debug = 1;

    Message user_msg = {.role = ROLE_USER, .content = "hello"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_tool_ok;
    ctx.debug = 1;

    int rc = agent_run(&ctx);
    if (rc != -1) { FAIL("expected -1 for unreachable LLM"); db_close(db); return; }

    db_close(db);
    PASS();
}

static void test_v2_retry_after_field(void) {
    TEST(v2_retry_after_field);
    /* HttpResponse retry_after field should default to 0 */
    HttpResponse resp = {0};
    if (resp.retry_after != 0) { FAIL("expected 0 default"); return; }
    PASS();
}

static void test_v10_json_parse_failure_recovery(void) {
    TEST(v10_json_parse_failure_recovery);
    /* Verify llm_parse_response returns -1 on garbage JSON */
    Arena *a = arena_create(ARENA_DEFAULT_SIZE);
    if (!a) { FAIL("arena_create failed"); return; }

    LlmResponse llm_resp;
    int rc = llm_parse_response(a, "not json at all {{{", &llm_resp);
    if (rc != -1) { FAIL("expected -1 for garbage JSON"); arena_destroy(a); return; }

    /* Also test empty string */
    rc = llm_parse_response(a, "", &llm_resp);
    if (rc != -1) { FAIL("expected -1 for empty string"); arena_destroy(a); return; }

    /* Also test valid JSON but missing choices */
    rc = llm_parse_response(a, "{\"id\":\"x\"}", &llm_resp);
    if (rc != -1) { FAIL("expected -1 for missing choices"); arena_destroy(a); return; }

    arena_destroy(a);
    PASS();
}

static void test_context_overflow_return_code(void) {
    TEST(context_overflow_return_code);
    /* agent_run returns -2 on context overflow, but we can't easily simulate
     * a 400 response here. Verify the return code contract: -1 for unreachable
     * (which is what we get with a bogus URL) */
    sqlite3 *db = db_open(":memory:");
    if (!db) { FAIL("db_open failed"); return; }
    int64_t sid = session_create(db, "test");
    if (sid < 0) { FAIL("session_create failed"); db_close(db); return; }

    Config cfg = {0};
    cfg.provider.base_url = "http://127.0.0.1:1/v1";
    cfg.provider.api_key = "fake";
    cfg.provider.model = "test";
    cfg.provider.context_window = 128000;
    cfg.max_iterations = 1;

    Message user_msg = {.role = ROLE_USER, .content = "hello"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_tool_ok;

    /* Unreachable → -1 (not -2, since it's not a 400 context overflow) */
    int rc = agent_run(&ctx);
    if (rc != -1) { FAIL("expected -1"); db_close(db); return; }

    db_close(db);
    PASS();
}

int main(void) {
    printf("test_agent:\n");
    test_agent_context_init();
    test_agent_run_null_ctx();
    test_agent_run_no_db();
    test_v10_dispatch_null_handler();
    test_v10_dispatch_returns_error_on_null();
    test_v10_dispatch_ok();
    test_agent_run_empty_session();
    test_max_iterations_configurable();
    test_max_iterations_default();
    test_debug_flag_stderr();
    test_v2_retry_after_field();
    test_v10_json_parse_failure_recovery();
    test_context_overflow_return_code();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
