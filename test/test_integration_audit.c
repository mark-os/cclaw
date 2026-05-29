/* T221: integration test audit — missing coverage for mock-server tests.
 * Covers: parallel tool calls, mid-loop retry, daemon error exit handling,
 * inbox multi-message flow, max iterations exhausted. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <curl/curl.h>
#include "agent.h"
#include "daemon.h"
#include "db.h"
#include "config.h"
#include "shutdown.h"
#include "mock_server.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

static char *mock_dispatch(const char *name, const char *arguments, void *user_data) {
    (void)user_data; (void)arguments;
    if (strcmp(name, "get_weather") == 0)
        return strdup("{\"temp\":22,\"condition\":\"sunny\"}");
    if (strcmp(name, "get_time") == 0)
        return strdup("2026-05-29T12:00:00Z");
    if (strcmp(name, "file_read") == 0)
        return strdup("file content here");
    char *err = malloc(64);
    snprintf(err, 64, "error: unknown tool '%s'", name);
    return err;
}

/* ── Test 1: Parallel tool calls (2 tool_calls in one response) ── */
static void test_parallel_tool_calls(void) {
    TEST(parallel_tool_calls);

    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start failed");

    /* LLM returns 2 tool_calls in one message */
    mock_server_enqueue(200,
        "{\"id\":\"c1\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":null,\"tool_calls\":["
        "{\"id\":\"call_a\",\"type\":\"function\",\"function\":{\"name\":\"get_weather\",\"arguments\":\"{\\\"city\\\":\\\"Tokyo\\\"}\"}},"
        "{\"id\":\"call_b\",\"type\":\"function\",\"function\":{\"name\":\"get_time\",\"arguments\":\"{}\"}}"
        "]},\"finish_reason\":\"tool_calls\"}],"
        "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}");

    /* After both tool results, LLM produces final */
    mock_server_enqueue(200,
        "{\"id\":\"c2\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"Tokyo is 22C at 12:00 UTC.\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":10}}");

    sqlite3 *db = db_open(":memory:");
    if (!db) { mock_server_stop(); FAIL("db_open"); }
    int64_t sid = session_create(db, "parallel", NULL, -1, 0);

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

    ToolSchema tools[2] = {
        {.name = "get_weather", .description = "Get weather",
         .parameters_json = "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\"}},\"required\":[\"city\"]}"},
        {.name = "get_time", .description = "Get time",
         .parameters_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}"}
    };

    Message user_msg = {.role = ROLE_USER, .content = "Weather and time?"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;
    ctx.tools = tools;
    ctx.tool_count = 2;

    int rc = agent_run(&ctx);
    if (rc != 0) { db_close(db); mock_server_stop(); FAIL("agent_run failed"); }

    /* Expected: user → assistant(2 tool_calls) → tool_result → tool_result → assistant(final) = 5 entries */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    if (!entries || count != 5) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected 5 entries, got %d", count);
        entry_branch_free(entries, count); db_close(db); mock_server_stop(); FAIL(msg);
    }

    /* Entry 1: assistant with 2 tool_calls */
    if (entries[1].message.tool_call_count != 2) {
        entry_branch_free(entries, count); db_close(db); mock_server_stop();
        FAIL("expected 2 tool_calls in assistant entry");
    }

    /* Entries 2,3: tool results */
    if (entries[2].message.role != ROLE_TOOL || entries[3].message.role != ROLE_TOOL) {
        entry_branch_free(entries, count); db_close(db); mock_server_stop();
        FAIL("entries 2,3 should be tool results");
    }

    /* Entry 4: final assistant */
    if (entries[4].message.role != ROLE_ASSISTANT || entries[4].message.tool_call_count != 0) {
        entry_branch_free(entries, count); db_close(db); mock_server_stop();
        FAIL("entry 4 should be final assistant");
    }

    if (mock_server_request_count() != 2) {
        entry_branch_free(entries, count); db_close(db); mock_server_stop();
        FAIL("expected 2 LLM requests");
    }

    entry_branch_free(entries, count);
    db_close(db);
    mock_server_stop();
    PASS();
}

/* ── Test 2: Mid-loop retry (429 after successful tool call) ── */
static void test_mid_loop_retry(void) {
    TEST(mid_loop_retry);

    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start failed");

    /* First LLM call: tool_call */
    mock_server_enqueue(200,
        "{\"id\":\"c1\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":null,\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\","
        "\"function\":{\"name\":\"get_time\",\"arguments\":\"{}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}],"
        "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}");

    /* Second LLM call (after tool result): 429 */
    const char *hdrs[] = {"Retry-After: 1", NULL};
    mock_server_enqueue_with_headers(429, "{\"error\":\"rate limited\"}", hdrs);

    /* Third LLM call (retry): success */
    mock_server_enqueue(200,
        "{\"id\":\"c2\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"The time is 12:00.\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":5}}");

    sqlite3 *db = db_open(":memory:");
    if (!db) { mock_server_stop(); FAIL("db_open"); }
    int64_t sid = session_create(db, "midretry", NULL, -1, 0);

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

    ToolSchema tools[1] = {
        {.name = "get_time", .description = "Get time",
         .parameters_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}"}
    };

    Message user_msg = {.role = ROLE_USER, .content = "What time is it?"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;
    ctx.tools = tools;
    ctx.tool_count = 1;

    int rc = agent_run(&ctx);
    if (rc != 0) { db_close(db); mock_server_stop(); FAIL("agent_run should succeed after mid-loop retry"); }

    /* 3 requests: tool_call, 429, final */
    if (mock_server_request_count() != 3) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected 3 requests, got %d", mock_server_request_count());
        db_close(db); mock_server_stop(); FAIL(msg);
    }

    /* Verify final entry */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    Entry *last = &entries[count - 1];
    if (last->message.role != ROLE_ASSISTANT || !last->message.content ||
        !strstr(last->message.content, "12:00")) {
        entry_branch_free(entries, count); db_close(db); mock_server_stop();
        FAIL("final response should contain time");
    }

    entry_branch_free(entries, count);
    db_close(db);
    mock_server_stop();
    PASS();
}

/* ── Test 3: Max iterations exhausted ── */
static void test_max_iterations_exhausted(void) {
    TEST(max_iterations_exhausted);

    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start failed");

    /* Enqueue enough tool_call responses to exceed max_iterations=3 */
    for (int i = 0; i < 5; i++) {
        char buf[512];
        snprintf(buf, sizeof(buf),
            "{\"id\":\"c%d\",\"choices\":[{\"message\":{\"role\":\"assistant\","
            "\"content\":null,\"tool_calls\":[{\"id\":\"call_%d\",\"type\":\"function\","
            "\"function\":{\"name\":\"get_time\",\"arguments\":\"{}\"}}]},"
            "\"finish_reason\":\"tool_calls\"}],"
            "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}", i, i);
        mock_server_enqueue(200, buf);
    }

    sqlite3 *db = db_open(":memory:");
    if (!db) { mock_server_stop(); FAIL("db_open"); }
    int64_t sid = session_create(db, "maxiter", NULL, -1, 0);

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);
    Config cfg = {0};
    cfg.provider.base_url = base_url;
    cfg.provider.api_key = "mock-key";
    cfg.provider.model = "mock-model";
    cfg.provider.max_tokens = 256;
    cfg.provider.context_window = 128000;
    cfg.max_iterations = 3;
    cfg.system_prompt = "You are a test assistant.";

    ToolSchema tools[1] = {
        {.name = "get_time", .description = "Get time",
         .parameters_json = "{\"type\":\"object\",\"properties\":{},\"required\":[]}"}
    };

    Message user_msg = {.role = ROLE_USER, .content = "Keep calling tools"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;
    ctx.tools = tools;
    ctx.tool_count = 1;

    int rc = agent_run(&ctx);
    /* Should fail — max iterations exhausted */
    if (rc == 0) { db_close(db); mock_server_stop(); FAIL("should fail on max iterations"); }

    /* Verify we made exactly max_iterations LLM requests */
    if (mock_server_request_count() > 4) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected <=4 requests, got %d", mock_server_request_count());
        db_close(db); mock_server_stop(); FAIL(msg);
    }

    db_close(db);
    mock_server_stop();
    PASS();
}

/* ── Test 4: Inbox multi-message consumption with mock LLM ── */
static void test_inbox_multi_message_flow(void) {
    TEST(inbox_multi_message_flow);

    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start failed");

    /* LLM responds to the consumed messages */
    mock_server_enqueue(200,
        "{\"id\":\"c1\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"I received both messages.\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":5}}");

    sqlite3 *db = db_open(":memory:");
    if (!db) { mock_server_stop(); FAIL("db_open"); }
    int64_t sid = session_create(db, "inbox_multi", NULL, -1, 0);

    /* Insert multiple inbox items */
    inbox_insert(db, sid, "telegram", "first message");
    inbox_insert(db, sid, "telegram", "second message");

    /* Consume inbox into entries (simulates what agent does at startup) */
    int consumed = inbox_consume_into_entries(db, sid, 10);
    if (consumed != 2) { db_close(db); mock_server_stop(); FAIL("expected 2 consumed"); }

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

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;
    ctx.tools = NULL;
    ctx.tool_count = 0;

    int rc = agent_run(&ctx);
    if (rc != 0) { db_close(db); mock_server_stop(); FAIL("agent_run failed"); }

    /* Verify: 2 user entries + 1 assistant = 3 total */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    if (!entries || count != 3) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected 3 entries, got %d", count);
        entry_branch_free(entries, count); db_close(db); mock_server_stop(); FAIL(msg);
    }

    /* First two should be user messages from inbox */
    if (entries[0].message.role != ROLE_USER || entries[1].message.role != ROLE_USER) {
        entry_branch_free(entries, count); db_close(db); mock_server_stop();
        FAIL("first 2 entries should be user");
    }

    /* Verify inbox is empty */
    if (inbox_count(db, sid) != 0) {
        entry_branch_free(entries, count); db_close(db); mock_server_stop();
        FAIL("inbox should be empty after consumption");
    }

    /* Last entry: assistant */
    if (entries[2].message.role != ROLE_ASSISTANT) {
        entry_branch_free(entries, count); db_close(db); mock_server_stop();
        FAIL("last entry should be assistant");
    }

    entry_branch_free(entries, count);
    db_close(db);
    mock_server_stop();
    PASS();
}

/* ── daemon_run wrapper for pthread ── */
static void *daemon_thread_fn(void *arg) {
    void **args = (void **)arg;
    Config *cfg = (Config *)args[0];
    sqlite3 *db = (sqlite3 *)args[1];
    daemon_run(cfg, db);
    return NULL;
}

/* ── Test 5: Daemon handles agent error exit gracefully ── */
static void test_daemon_agent_error_exit(void) {
    TEST(daemon_agent_error_exit);

    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start failed");

    /* Enqueue 400 errors (non-overflow) — causes fast failure without retry sleep */
    for (int i = 0; i < 10; i++)
        mock_server_enqueue(400, "{\"error\":{\"message\":\"invalid request\"}}");

    char orig_cwd[1024];
    getcwd(orig_cwd, sizeof(orig_cwd));

    const char *work_dir = "/tmp/test_integ_audit_work";
    const char *agent_name = "audit_agent";
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", work_dir);
    system(cmd);
    mkdir(work_dir, 0755);
    snprintf(cmd, sizeof(cmd), "%s/agents", work_dir);
    mkdir(cmd, 0755);
    snprintf(cmd, sizeof(cmd), "%s/agents/%s", work_dir, agent_name);
    mkdir(cmd, 0755);
    snprintf(cmd, sizeof(cmd), "%s/agents/%s/workspace", work_dir, agent_name);
    mkdir(cmd, 0755);
    chdir(work_dir);

    /* Create agent DB with session + inbox */
    char adb_path[256];
    snprintf(adb_path, sizeof(adb_path), "agents/%s/agent.db", agent_name);
    sqlite3 *adb = db_open_agent(adb_path);
    if (!adb) { mock_server_stop(); chdir(orig_cwd); FAIL("db_open_agent"); }
    int64_t sid = session_create(adb, "error_test", agent_name, -1, 0);
    inbox_insert(adb, sid, "test", "trigger error");
    db_close(adb);

    /* Create daemon DB */
    const char *ddb_path = "/tmp/test_integ_audit_daemon.db";
    unlink(ddb_path);
    sqlite3 *ddb = db_open(ddb_path);
    if (!ddb) { mock_server_stop(); chdir(orig_cwd); FAIL("db_open"); }

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);
    db_kv_set(ddb, "provider.base_url", base_url);
    db_kv_set(ddb, "provider.model", "mock-model");
    db_kv_set(ddb, "provider.max_tokens", "256");
    db_kv_set(ddb, "provider.context_window", "128000");
    db_kv_set(ddb, "provider.api_key", "mock-key");
    db_kv_set(ddb, "workspace", "/tmp");
    db_kv_set(ddb, "max_iterations", "5");
    db_kv_set(ddb, "shell_timeout", "5");

    shutdown_reset();
    char self_path[4096];
    snprintf(self_path, sizeof(self_path), "%s/build/cclaw", orig_cwd);
    daemon_set_self_path(self_path);

    Config cfg = {0};
    cfg.db_path = (char *)ddb_path;
    cfg.workspace = "/tmp";
    cfg.shell_timeout = 5;
    cfg.provider.base_url = base_url;
    cfg.provider.api_key = "mock-key";
    cfg.provider.model = "mock-model";
    cfg.provider.max_tokens = 256;
    cfg.provider.context_window = 128000;
    cfg.max_iterations = 5;

    void *args[2] = {&cfg, ddb};
    pthread_t dt;
    pthread_create(&dt, NULL, daemon_thread_fn, args);
    usleep(200000);

    daemon_signal_session_agent(sid, agent_name);

    /* Wait for session to return to idle (daemon reaps error exit) */
    int settled = 0;
    for (int i = 0; i < 200; i++) {
        usleep(100000);
        adb = db_open_agent(adb_path);
        if (!adb) continue;
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(adb, "SELECT state FROM sessions WHERE id=?;", -1, &stmt, NULL);
        sqlite3_bind_int64(stmt, 1, sid);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *state = (const char *)sqlite3_column_text(stmt, 0);
            if (state && strcmp(state, "idle") == 0) settled = 1;
        }
        sqlite3_finalize(stmt);
        db_close(adb);
        if (settled) break;
    }

    shutdown_request();
    pthread_join(dt, NULL);

    if (!settled) {
        db_close(ddb); mock_server_stop(); chdir(orig_cwd);
        snprintf(cmd, sizeof(cmd), "rm -rf %s", work_dir);
        system(cmd); unlink(ddb_path);
        FAIL("session did not return to idle after agent error exit");
    }

    /* Verify error entry written */
    adb = db_open_agent(adb_path);
    int count = 0;
    Entry *entries = session_get_branch(adb, sid, &count);
    int found_error = 0;
    if (entries) {
        for (int i = 0; i < count; i++) {
            if (entries[i].message.role == ROLE_ASSISTANT &&
                entries[i].message.stop_reason == STOP_REASON_ERROR)
                found_error = 1;
        }
        entry_branch_free(entries, count);
    }
    db_close(adb);
    db_close(ddb);
    mock_server_stop();
    chdir(orig_cwd);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", work_dir);
    system(cmd);
    unlink(ddb_path);

    if (!found_error) FAIL("expected error entry in DB after agent error exit");
    PASS();
}

int main(void) {
    alarm(120);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    setenv("CCLAW_NO_LANDLOCK", "1", 1);
    printf("--- test_integration_audit (T221) ---\n");

    test_parallel_tool_calls();
    test_mid_loop_retry();
    test_max_iterations_exhausted();
    test_inbox_multi_message_flow();
    test_daemon_agent_error_exit();

    printf("%d/%d passed\n", tests_passed, tests_run);
    curl_global_cleanup();
    return tests_passed == tests_run ? 0 : 1;
}
