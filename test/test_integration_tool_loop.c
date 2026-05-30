/* T183: integration test — tool loop detection.
 * Mock returns same tool_call 12×; verify warning injected at rep 5;
 * verify agent stops at rep 10 w/ error. Cites: V43. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "agent.h"
#include "db.h"
#include "config.h"
#include "mock_server.h"
static int s_port;

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

/* Track tool dispatch calls */
static int dispatch_count;

static char *mock_dispatch(const char *name, const char *arguments, void *user_data) {
    (void)user_data; (void)name; (void)arguments;
    dispatch_count++;
    return strdup("file content here");
}

/* Helper: enqueue a tool_call response (always same call: file_read with path "test.c") */
static void enqueue_tool_call(void) {
    mock_server_enqueue(200,
        "{\"id\":\"c1\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":null,\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\","
        "\"function\":{\"name\":\"file_read\",\"arguments\":\"{\\\"path\\\":\\\"test.c\\\"}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}],\"usage\":{\"prompt_tokens\":10,"
        "\"completion_tokens\":5,\"total_tokens\":15}}");
}

/* V43: warning at rep 5, stop at rep 10 */
static void test_tool_loop_stops_at_10(void) {
    TEST(tool_loop_stops_at_10);

    mock_server_reset();

    /* Enqueue 12 identical tool_call responses (agent should stop at 10) */
    for (int i = 0; i < 12; i++)
        enqueue_tool_call();

    sqlite3 *db = db_open(":memory:");
    if (!db) { FAIL("db_open failed"); }

    int64_t sid = session_create(db, "loop_test", NULL, -1, 0);
    if (sid < 0) { db_close(db); FAIL("session_create failed"); }

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", s_port);

    Config cfg = {0};
    cfg.provider.base_url = base_url;
    cfg.provider.api_key = "mock-key";
    cfg.provider.model = "mock-model";
    cfg.provider.max_tokens = 256;
    cfg.provider.context_window = 128000;
    cfg.max_iterations = 20;
    cfg.system_prompt = "You are a helpful assistant.";

    ToolSchema tools[1] = {{
        .name = "file_read",
        .description = "Read a file",
        .parameters_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}"
    }};

    Message user_msg = {.role = ROLE_USER, .content = "Read test.c"};
    entry_append(db, sid, &user_msg);

    dispatch_count = 0;

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;
    ctx.tools = tools;
    ctx.tool_count = 1;

    int rc = agent_run(&ctx);
    /* Should return -1 (tool loop forced stop) */
    if (rc != -1) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected rc=-1, got %d", rc);
        db_close(db); FAIL(msg);
    }

    /* Tool dispatched 9 times (reps 1-4 normal, 5-9 with warning, rep 10 = break before dispatch) */
    if (dispatch_count != 9) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected 9 dispatches, got %d", dispatch_count);
        db_close(db); FAIL(msg);
    }

    /* Verify entries: check that warning text appears in tool results for reps 5-9 */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    if (!entries || count < 2) {
        entry_branch_free(entries, count);
        db_close(db); FAIL("too few entries");
    }

    /* Find tool result entries and check for warning text */
    int warned = 0;
    int loop_error = 0;
    for (int i = 0; i < count; i++) {
        if (entries[i].message.role == ROLE_TOOL && entries[i].message.tool_result) {
            const char *content = entries[i].message.tool_result->content;
            if (content && strstr(content, "WARNING"))
                warned++;
            if (content && strstr(content, "tool loop detected"))
                loop_error = 1;
        }
    }

    if (warned < 1) {
        entry_branch_free(entries, count);
        db_close(db); FAIL("no warning found in tool results");
    }

    if (!loop_error) {
        entry_branch_free(entries, count);
        db_close(db); FAIL("no 'tool loop detected' error found");
    }

    /* Verify LLM was called exactly 10 times (not 12 — stopped early) */
    if (mock_server_request_count() != 10) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected 10 LLM requests, got %d", mock_server_request_count());
        entry_branch_free(entries, count);
        db_close(db); FAIL(msg);
    }

    entry_branch_free(entries, count);
    db_close(db);
    PASS();
}

/* V43: warning injected at exactly rep 5 */
static void test_warning_at_rep_5(void) {
    TEST(warning_at_rep_5);

    mock_server_reset();

    /* Enqueue 5 identical tool_call responses, then a final stop response */
    for (int i = 0; i < 5; i++)
        enqueue_tool_call();
    mock_server_enqueue(200,
        "{\"id\":\"c6\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"Done.\"},"
        "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":10,"
        "\"completion_tokens\":3,\"total_tokens\":13}}");

    sqlite3 *db = db_open(":memory:");
    if (!db) { FAIL("db_open failed"); }

    int64_t sid = session_create(db, "warn_test", NULL, -1, 0);
    if (sid < 0) { db_close(db); FAIL("session_create failed"); }

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", s_port);

    Config cfg = {0};
    cfg.provider.base_url = base_url;
    cfg.provider.api_key = "mock-key";
    cfg.provider.model = "mock-model";
    cfg.provider.max_tokens = 256;
    cfg.provider.context_window = 128000;
    cfg.max_iterations = 20;
    cfg.system_prompt = "You are a helpful assistant.";

    ToolSchema tools[1] = {{
        .name = "file_read",
        .description = "Read a file",
        .parameters_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}"
    }};

    Message user_msg = {.role = ROLE_USER, .content = "Read test.c"};
    entry_append(db, sid, &user_msg);

    dispatch_count = 0;

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;
    ctx.tools = tools;
    ctx.tool_count = 1;

    int rc = agent_run(&ctx);
    /* Should succeed (LLM stops after 5 tool calls) */
    if (rc != 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected rc=0, got %d", rc);
        db_close(db); FAIL(msg);
    }

    /* All 5 tool calls dispatched */
    if (dispatch_count != 5) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected 5 dispatches, got %d", dispatch_count);
        db_close(db); FAIL(msg);
    }

    /* Check that the 5th tool result has the warning */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    if (!entries) { db_close(db); FAIL("no entries"); }

    int tool_results_seen = 0;
    int fifth_has_warning = 0;
    int fourth_no_warning = 1;
    for (int i = 0; i < count; i++) {
        if (entries[i].message.role == ROLE_TOOL && entries[i].message.tool_result) {
            tool_results_seen++;
            const char *content = entries[i].message.tool_result->content;
            if (tool_results_seen == 5 && content && strstr(content, "WARNING"))
                fifth_has_warning = 1;
            if (tool_results_seen == 4 && content && strstr(content, "WARNING"))
                fourth_no_warning = 0;
        }
    }

    if (!fourth_no_warning) {
        entry_branch_free(entries, count);
        db_close(db); FAIL("4th result should NOT have warning");
    }

    if (!fifth_has_warning) {
        entry_branch_free(entries, count);
        db_close(db); FAIL("5th result should have warning");
    }

    entry_branch_free(entries, count);
    db_close(db);
    PASS();
}

/* V43: different tool calls don't trigger loop detection */
static void test_no_false_positive(void) {
    TEST(no_false_positive_different_calls);

    mock_server_reset();

    /* 6 tool calls with different arguments each time */
    for (int i = 0; i < 6; i++) {
        char body[512];
        snprintf(body, sizeof(body),
            "{\"id\":\"c%d\",\"choices\":[{\"message\":{\"role\":\"assistant\","
            "\"content\":null,\"tool_calls\":[{\"id\":\"call_%d\",\"type\":\"function\","
            "\"function\":{\"name\":\"file_read\",\"arguments\":\"{\\\"path\\\":\\\"file%d.c\\\"}\"}}]},"
            "\"finish_reason\":\"tool_calls\"}],\"usage\":{\"prompt_tokens\":10,"
            "\"completion_tokens\":5,\"total_tokens\":15}}", i, i, i);
        mock_server_enqueue(200, body);
    }
    /* Final stop */
    mock_server_enqueue(200,
        "{\"id\":\"c7\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"All done.\"},"
        "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":10,"
        "\"completion_tokens\":3,\"total_tokens\":13}}");

    sqlite3 *db = db_open(":memory:");
    if (!db) { FAIL("db_open failed"); }

    int64_t sid = session_create(db, "no_fp_test", NULL, -1, 0);
    if (sid < 0) { db_close(db); FAIL("session_create failed"); }

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", s_port);

    Config cfg = {0};
    cfg.provider.base_url = base_url;
    cfg.provider.api_key = "mock-key";
    cfg.provider.model = "mock-model";
    cfg.provider.max_tokens = 256;
    cfg.provider.context_window = 128000;
    cfg.max_iterations = 20;
    cfg.system_prompt = "You are a helpful assistant.";

    ToolSchema tools[1] = {{
        .name = "file_read",
        .description = "Read a file",
        .parameters_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"]}"
    }};

    Message user_msg = {.role = ROLE_USER, .content = "Read all files"};
    entry_append(db, sid, &user_msg);

    dispatch_count = 0;

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;
    ctx.tools = tools;
    ctx.tool_count = 1;

    int rc = agent_run(&ctx);
    if (rc != 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected rc=0, got %d", rc);
        db_close(db); FAIL(msg);
    }

    /* All 6 dispatched, no warnings */
    if (dispatch_count != 6) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected 6 dispatches, got %d", dispatch_count);
        db_close(db); FAIL(msg);
    }

    /* Verify no WARNING in any tool result */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    for (int i = 0; i < count; i++) {
        if (entries[i].message.role == ROLE_TOOL && entries[i].message.tool_result) {
            if (strstr(entries[i].message.tool_result->content, "WARNING")) {
                entry_branch_free(entries, count);
                db_close(db);
                FAIL("unexpected WARNING in tool result");
            }
        }
    }

    entry_branch_free(entries, count);
    db_close(db);
    PASS();
}

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    s_port = mock_server_start();
    printf("--- test_integration_tool_loop (T183) ---\n");
    test_tool_loop_stops_at_10();
    test_warning_at_rep_5();
    test_no_false_positive();
    printf("%d/%d passed\n", tests_passed, tests_run);
    curl_global_cleanup();
    mock_server_stop();
    return tests_passed == tests_run ? 0 : 1;
}
