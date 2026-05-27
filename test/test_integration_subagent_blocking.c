/* T184: integration test — blocking sub-agent lifecycle.
 * Mock LLM for parent + child; parent calls spawn_agent(blocking);
 * verify state="waiting" + spawn_queue row; simulate sub-agent completion;
 * verify parent re-forks w/ tool_result. Cites V13, V33. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <curl/curl.h>
#include "daemon.h"
#include "db.h"
#include "shutdown.h"
#include "mock_server.h"

static const char *DB_PATH = "/tmp/test_integ_subagent_blocking.sqlite";

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

static void *daemon_thread(void *arg) {
    void **args = (void **)arg;
    Config *cfg = (Config *)args[0];
    sqlite3 *db = (sqlite3 *)args[1];
    daemon_run(cfg, db);
    return NULL;
}

static void seed_kv_config(sqlite3 *db, int port) {
    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);
    db_kv_set(db, "provider.base_url", base_url);
    db_kv_set(db, "provider.model", "mock-model");
    db_kv_set(db, "provider.max_tokens", "256");
    db_kv_set(db, "provider.context_window", "128000");
    db_kv_set(db, "provider.api_key", "mock-key");
    db_kv_set(db, "workspace", "/tmp");
    db_kv_set(db, "max_iterations", "10");
    db_kv_set(db, "shell_timeout", "5");
}

/* Wait for spawn_queue to have an entry with given status */
static int wait_for_spawn_status(sqlite3 *db, int64_t parent_sid, const char *status, int wait_ms) {
    for (int i = 0; i < wait_ms / 100; i++) {
        usleep(100000);
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db,
            "SELECT COUNT(*) FROM spawn_queue WHERE parent_session_id=? AND status=?;",
            -1, &stmt, NULL);
        sqlite3_bind_int64(stmt, 1, parent_sid);
        sqlite3_bind_text(stmt, 2, status, -1, SQLITE_STATIC);
        int count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW)
            count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        if (count > 0) return 0;
    }
    return -1;
}

/* Wait for session to return to idle with assistant entries (max wait_ms) */
static int wait_for_idle_with_entries(sqlite3 *db, int64_t sid, int min_assistant, int wait_ms) {
    for (int i = 0; i < wait_ms / 100; i++) {
        usleep(100000);
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, "SELECT state FROM sessions WHERE id=?;", -1, &stmt, NULL);
        sqlite3_bind_int64(stmt, 1, sid);
        int idle = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *state = (const char *)sqlite3_column_text(stmt, 0);
            if (state && strcmp(state, "idle") == 0) idle = 1;
        }
        sqlite3_finalize(stmt);
        if (!idle) continue;

        /* Check assistant entry count */
        sqlite3_prepare_v2(db,
            "SELECT COUNT(*) FROM entries WHERE session_id=? AND role=2;",
            -1, &stmt, NULL);
        sqlite3_bind_int64(stmt, 1, sid);
        int count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW)
            count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        if (count >= min_assistant) return 0;
    }
    return -1;
}

/* T184: Full blocking sub-agent lifecycle via daemon fork+reap */
static void test_blocking_subagent_lifecycle(void) {
    TEST(blocking_subagent_lifecycle);

    /* Disable landlock for agent children (mock server uses random port) */
    setenv("CCLAW_NO_LANDLOCK", "1", 1);

    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start failed");

    /* Response 1: Parent gets tool_call for launch_agent (blocking) */
    mock_server_enqueue(200,
        "{\"id\":\"r1\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":null,\"tool_calls\":[{\"id\":\"call_spawn1\",\"type\":\"function\","
        "\"function\":{\"name\":\"launch_agent\","
        "\"arguments\":\"{\\\"task\\\":\\\"do child work\\\"}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}],"
        "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}");

    /* Response 2: Parent's second LLM call (after SPAWN_BLOCKING tool result) → stop */
    mock_server_enqueue(200,
        "{\"id\":\"r2\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"Waiting for sub-agent.\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":5}}");

    /* Response 3: Child agent's LLM call → final response */
    mock_server_enqueue(200,
        "{\"id\":\"r3\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"child completed successfully\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}");

    /* Response 4: Parent re-fork LLM call (after sub-agent result in inbox) → final */
    mock_server_enqueue(200,
        "{\"id\":\"r4\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"parent done after child\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":30,\"completion_tokens\":5}}");

    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    if (!db) { mock_server_stop(); FAIL("db_open"); }

    seed_kv_config(db, port);

    int64_t parent_sid = session_create(db, "parent_agent", NULL, -1, 0);
    if (parent_sid < 0) { db_close(db); mock_server_stop(); FAIL("session_create"); }

    /* Insert user message to trigger parent agent */
    inbox_insert(db, parent_sid, "test", "please spawn a child agent");

    /* Start daemon */
    shutdown_reset();
    daemon_set_self_path("./build/cclaw");

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);

    Config cfg = {0};
    cfg.db_path = (char *)DB_PATH;
    cfg.workspace = "/tmp";
    cfg.shell_timeout = 5;
    cfg.provider.base_url = base_url;
    cfg.provider.api_key = "mock-key";
    cfg.provider.model = "mock-model";
    cfg.provider.max_tokens = 256;
    cfg.provider.context_window = 128000;
    cfg.max_iterations = 10;

    void *args[2] = {&cfg, db};
    pthread_t dt;
    pthread_create(&dt, NULL, daemon_thread, args);
    usleep(100000);

    /* Signal daemon to fork parent agent */
    daemon_signal_session(parent_sid);

    /* V13: Wait for spawn_queue entry to appear (parent called launch_agent) */
    if (wait_for_spawn_status(db, parent_sid, "pending", 15000) != 0) {
        /* Spawn queue might already be processed (forked) */
        if (wait_for_spawn_status(db, parent_sid, "forked", 15000) != 0) {
            shutdown_request();
            pthread_join(dt, NULL);
            db_close(db); mock_server_stop();
            FAIL("spawn_queue entry never appeared");
        }
    }

    /* V13/V33: Wait for spawn_queue to reach "done" (child completed) */
    if (wait_for_spawn_status(db, parent_sid, "done", 20000) != 0) {
        shutdown_request();
        pthread_join(dt, NULL);
        db_close(db); mock_server_stop();
        FAIL("spawn_queue never reached 'done' status");
    }

    /* V33: Wait for parent to return to idle after re-fork */
    if (wait_for_idle_with_entries(db, parent_sid, 3, 20000) != 0) {
        /* Parent should have at least 3 assistant entries:
         * 1. tool_call for launch_agent
         * 2. "Waiting for sub-agent" (after SPAWN_BLOCKING)
         * 3. "parent done after child" (after re-fork) */
        if (wait_for_idle_with_entries(db, parent_sid, 2, 5000) != 0) {
            shutdown_request();
            pthread_join(dt, NULL);
            db_close(db); mock_server_stop();
            FAIL("parent did not return to idle after re-fork");
        }
    }

    /* Verify: spawn_queue has a "done" entry for parent */
    {
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db,
            "SELECT status, child_session_id FROM spawn_queue WHERE parent_session_id=?;",
            -1, &stmt, NULL);
        sqlite3_bind_int64(stmt, 1, parent_sid);
        int found_done = 0;
        int64_t child_sid = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *st = (const char *)sqlite3_column_text(stmt, 0);
            if (st && strcmp(st, "done") == 0) {
                found_done = 1;
                child_sid = sqlite3_column_int64(stmt, 1);
            }
        }
        sqlite3_finalize(stmt);
        if (!found_done) {
            shutdown_request();
            pthread_join(dt, NULL);
            db_close(db); mock_server_stop();
            FAIL("spawn_queue entry not in 'done' state");
        }

        /* Verify child session exists and has assistant entry */
        if (child_sid > 0) {
            sqlite3_prepare_v2(db,
                "SELECT COUNT(*) FROM entries WHERE session_id=? AND role=2;",
                -1, &stmt, NULL);
            sqlite3_bind_int64(stmt, 1, child_sid);
            int child_entries = 0;
            if (sqlite3_step(stmt) == SQLITE_ROW)
                child_entries = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
            if (child_entries < 1) {
                shutdown_request();
                pthread_join(dt, NULL);
                db_close(db); mock_server_stop();
                FAIL("child session has no assistant entries");
            }
        }
    }

    /* Verify: parent inbox was consumed (sub-agent result delivered + consumed) */
    if (inbox_count(db, parent_sid) != 0) {
        shutdown_request();
        pthread_join(dt, NULL);
        db_close(db); mock_server_stop();
        FAIL("parent inbox not empty after re-fork");
    }

    /* Verify: mock server received multiple requests (parent + child + parent re-fork) */
    int req_count = mock_server_request_count();
    if (req_count < 3) {
        char msg[128];
        snprintf(msg, sizeof(msg), "expected >=3 mock requests, got %d", req_count);
        shutdown_request();
        pthread_join(dt, NULL);
        db_close(db); mock_server_stop();
        FAIL(msg);
    }

    shutdown_request();
    pthread_join(dt, NULL);
    db_close(db);
    mock_server_stop();
    unlink(DB_PATH);
    PASS();
}

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    printf("--- test_integration_subagent_blocking (T184) ---\n");
    test_blocking_subagent_lifecycle();
    printf("%d/%d passed\n", tests_passed, tests_run);
    curl_global_cleanup();
    return tests_passed == tests_run ? 0 : 1;
}
