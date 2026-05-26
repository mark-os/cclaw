/* T130: integration test — daemon fork+reap with mock LLM.
 * Daemon forks agent process, agent hits mock HTTP server, writes response,
 * daemon reaps and delivers. Verifies V21 (daemon forks), V26 (delivers). */
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

static const char *DB_PATH = "/tmp/test_integ_daemon.sqlite";
static const char *CFG_PATH = "/tmp/test_integ_daemon_config.json";

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

static void write_config(int port) {
    FILE *f = fopen(CFG_PATH, "w");
    assert(f);
    fprintf(f,
        "{\"db_path\":\"%s\",\"workspace\":\"/tmp\","
        "\"provider\":{\"base_url\":\"http://127.0.0.1:%d/v1\","
        "\"api_key\":\"mock-key\",\"model\":\"mock-model\","
        "\"max_tokens\":256,\"context_window\":128000},"
        "\"max_iterations\":5}",
        DB_PATH, port);
    fclose(f);
}

/* Wait for session to return to idle with an assistant entry (max 15s) */
static int wait_for_completion(sqlite3 *db, int64_t sid) {
    for (int i = 0; i < 150; i++) {
        usleep(100000);
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db,
            "SELECT state FROM sessions WHERE id=?;", -1, &stmt, NULL);
        sqlite3_bind_int64(stmt, 1, sid);
        int done = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *state = (const char *)sqlite3_column_text(stmt, 0);
            if (state && strcmp(state, "idle") == 0) {
                sqlite3_finalize(stmt);
                /* Check for assistant entry */
                sqlite3_prepare_v2(db,
                    "SELECT COUNT(*) FROM entries WHERE session_id=? AND role=2;",
                    -1, &stmt, NULL);
                sqlite3_bind_int64(stmt, 1, sid);
                if (sqlite3_step(stmt) == SQLITE_ROW &&
                    sqlite3_column_int(stmt, 0) > 0)
                    done = 1;
            }
        }
        sqlite3_finalize(stmt);
        if (done) return 0;
    }
    return -1;
}

/* T130: daemon forks agent, agent hits mock LLM, daemon reaps + delivers */
static void test_daemon_fork_reap_mock(void) {
    TEST(daemon_fork_reap_mock);

    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start failed");

    /* Enqueue a simple final response */
    mock_server_enqueue(200,
        "{\"id\":\"chatcmpl-d1\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"Hello from mock LLM\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}");

    write_config(port);

    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    if (!db) { mock_server_stop(); FAIL("db_open"); }

    int64_t sid = session_create(db, "integ_daemon", NULL, -1, 0);
    if (sid < 0) { db_close(db); mock_server_stop(); FAIL("session_create"); }

    /* Insert inbox item to trigger agent */
    inbox_insert(db, sid, "test", "hi there");

    /* Configure daemon */
    shutdown_reset();
    daemon_set_self_path("./build/cclaw");
    daemon_set_config_path(CFG_PATH);

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
    cfg.max_iterations = 5;

    void *args[2] = {&cfg, db};
    pthread_t dt;
    pthread_create(&dt, NULL, daemon_thread, args);
    usleep(100000); /* let daemon start */

    daemon_signal_session(sid);

    if (wait_for_completion(db, sid) != 0) {
        shutdown_request();
        pthread_join(dt, NULL);
        db_close(db); mock_server_stop();
        FAIL("agent did not complete within timeout");
    }

    /* Verify mock server received at least 1 request */
    if (mock_server_request_count() < 1) {
        shutdown_request();
        pthread_join(dt, NULL);
        db_close(db); mock_server_stop();
        FAIL("mock server received no requests");
    }

    /* Verify inbox consumed */
    if (inbox_count(db, sid) != 0) {
        shutdown_request();
        pthread_join(dt, NULL);
        db_close(db); mock_server_stop();
        FAIL("inbox not consumed");
    }

    /* Verify assistant entry in DB */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    int found_assistant = 0;
    if (entries) {
        for (int i = 0; i < count; i++) {
            if (entries[i].message.role == ROLE_ASSISTANT &&
                entries[i].message.content &&
                strstr(entries[i].message.content, "Hello from mock LLM")) {
                found_assistant = 1;
                break;
            }
        }
        entry_branch_free(entries, count);
    }
    if (!found_assistant) {
        shutdown_request();
        pthread_join(dt, NULL);
        db_close(db); mock_server_stop();
        FAIL("assistant entry not found in DB");
    }

    shutdown_request();
    pthread_join(dt, NULL);
    db_close(db);
    mock_server_stop();
    unlink(DB_PATH);
    unlink(CFG_PATH);
    PASS();
}

/* T130: daemon fork+reap with tool call — agent makes 2 LLM requests */
static void test_daemon_fork_reap_tool_call(void) {
    TEST(daemon_fork_reap_tool_call);

    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start failed");

    /* First response: tool call */
    mock_server_enqueue(200,
        "{\"id\":\"chatcmpl-d2\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":null,\"tool_calls\":[{\"id\":\"call_d1\",\"type\":\"function\","
        "\"function\":{\"name\":\"file_read\",\"arguments\":\"{\\\"path\\\":\\\"/tmp/test_integ_daemon_file.txt\\\"}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}");

    /* Second response: final answer */
    mock_server_enqueue(200,
        "{\"id\":\"chatcmpl-d3\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"File contents received.\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":5}}");

    write_config(port);

    /* Create the file the agent will try to read */
    FILE *tf = fopen("/tmp/test_integ_daemon_file.txt", "w");
    if (tf) { fprintf(tf, "test data"); fclose(tf); }

    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    if (!db) { mock_server_stop(); FAIL("db_open"); }

    int64_t sid = session_create(db, "integ_daemon_tool", NULL, -1, 0);
    if (sid < 0) { db_close(db); mock_server_stop(); FAIL("session_create"); }

    inbox_insert(db, sid, "test", "read the file");

    shutdown_reset();
    daemon_set_self_path("./build/cclaw");
    daemon_set_config_path(CFG_PATH);

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
    cfg.max_iterations = 5;

    void *args[2] = {&cfg, db};
    pthread_t dt;
    pthread_create(&dt, NULL, daemon_thread, args);
    usleep(100000);

    daemon_signal_session(sid);

    if (wait_for_completion(db, sid) != 0) {
        shutdown_request();
        pthread_join(dt, NULL);
        db_close(db); mock_server_stop();
        FAIL("agent did not complete within timeout");
    }

    /* Mock should have received 2 requests (tool_call + final) */
    if (mock_server_request_count() < 2) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected >=2 requests, got %d",
                 mock_server_request_count());
        shutdown_request();
        pthread_join(dt, NULL);
        db_close(db); mock_server_stop();
        FAIL(msg);
    }

    /* Verify final assistant entry */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    int found = 0;
    if (entries) {
        for (int i = 0; i < count; i++) {
            if (entries[i].message.role == ROLE_ASSISTANT &&
                entries[i].message.content &&
                strstr(entries[i].message.content, "File contents received")) {
                found = 1;
                break;
            }
        }
        entry_branch_free(entries, count);
    }
    if (!found) {
        shutdown_request();
        pthread_join(dt, NULL);
        db_close(db); mock_server_stop();
        FAIL("final assistant entry not found");
    }

    shutdown_request();
    pthread_join(dt, NULL);
    db_close(db);
    mock_server_stop();
    unlink(DB_PATH);
    unlink(CFG_PATH);
    unlink("/tmp/test_integ_daemon_file.txt");
    PASS();
}

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    printf("--- test_integration_daemon (T130) ---\n");
    test_daemon_fork_reap_mock();
    test_daemon_fork_reap_tool_call();
    printf("%d/%d passed\n", tests_passed, tests_run);
    curl_global_cleanup();
    return tests_passed == tests_run ? 0 : 1;
}
