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
#include <sys/stat.h>
#include <curl/curl.h>
#include "daemon.h"
#include "db.h"
#include "shutdown.h"
#include "mock_server.h"
static int s_port;

static const char *DB_PATH = "/tmp/test_integ_daemon.sqlite";
static const char *WORK_DIR = "/tmp/test_integ_daemon_work";
static const char *AGENT_NAME = "integ_agent";

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

/* Seed kv table with config pointing to mock server */
static void seed_kv_config(sqlite3 *db) {
    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", s_port);
    db_kv_set(db, "provider.base_url", base_url);
    db_kv_set(db, "provider.model", "mock-model");
    db_kv_set(db, "provider.max_tokens", "256");
    db_kv_set(db, "provider.context_window", "128000");
    db_kv_set(db, "workspace", "/tmp");
    db_kv_set(db, "max_iterations", "5");
    db_kv_set(db, "shell_timeout", "5");
}

/* T200: Setup agent dir + DB for integration tests */
static void setup_agent_dir(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", WORK_DIR);
    system(cmd);
    mkdir(WORK_DIR, 0755);
    snprintf(cmd, sizeof(cmd), "%s/agents", WORK_DIR);
    mkdir(cmd, 0755);
    snprintf(cmd, sizeof(cmd), "%s/agents/%s", WORK_DIR, AGENT_NAME);
    mkdir(cmd, 0755);
    snprintf(cmd, sizeof(cmd), "%s/agents/%s/workspace", WORK_DIR, AGENT_NAME);
    mkdir(cmd, 0755);
}

/* Wait for session to return to idle with an assistant entry (max 15s) */
static int wait_for_completion(const char *agent_db_path, int64_t sid) {
    for (int i = 0; i < 150; i++) {
        usleep(100000);
        sqlite3 *adb = db_open_agent(agent_db_path);
        if (!adb) continue;
        sqlite3_stmt *stmt;
        int done = 0;
        sqlite3_prepare_v2(adb,
            "SELECT state FROM sessions WHERE id=?;", -1, &stmt, NULL);
        sqlite3_bind_int64(stmt, 1, sid);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *state = (const char *)sqlite3_column_text(stmt, 0);
            if (state && strcmp(state, "idle") == 0) {
                sqlite3_finalize(stmt);
                sqlite3_prepare_v2(adb,
                    "SELECT COUNT(*) FROM entries WHERE session_id=? AND role=2;",
                    -1, &stmt, NULL);
                sqlite3_bind_int64(stmt, 1, sid);
                if (sqlite3_step(stmt) == SQLITE_ROW &&
                    sqlite3_column_int(stmt, 0) > 0)
                    done = 1;
            }
        }
        sqlite3_finalize(stmt);
        db_close(adb);
        if (done) return 0;
    }
    return -1;
}

/* T130: daemon forks agent, agent hits mock LLM, daemon reaps + delivers */
static void test_daemon_fork_reap_mock(void) {
    TEST(daemon_fork_reap_mock);

    mock_server_reset();

    mock_server_enqueue(200,
        "{\"id\":\"chatcmpl-d1\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"Hello from mock LLM\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}");

    char orig_cwd[1024];
    getcwd(orig_cwd, sizeof(orig_cwd));
    setup_agent_dir();
    chdir(WORK_DIR);

    /* Create agent DB with session + inbox */
    char adb_path[256];
    snprintf(adb_path, sizeof(adb_path), "agents/%s/agent.db", AGENT_NAME);
    sqlite3 *adb = db_open_agent(adb_path);
    if (!adb) { chdir(orig_cwd); FAIL("db_open_agent"); }
    int64_t sid = session_create(adb, "integ_daemon", AGENT_NAME, -1, 0);
    if (sid < 0) { db_close(adb); chdir(orig_cwd); FAIL("session_create"); }
    inbox_insert(adb, sid, "test", "hi there");
    db_close(adb);

    /* Create daemon DB */
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    if (!db) { chdir(orig_cwd); FAIL("db_open"); }
    seed_kv_config(db);
    db_kv_set(db, "provider.api_key", "mock-key");

    shutdown_reset();
    char self_path[4096];
    snprintf(self_path, sizeof(self_path), "%s/build/cclaw", orig_cwd);
    daemon_set_self_path(self_path);

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", s_port);
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

    daemon_signal_session_agent(sid, AGENT_NAME);

    if (wait_for_completion(adb_path, sid) != 0) {
        shutdown_request();
        pthread_join(dt, NULL);
        db_close(db); chdir(orig_cwd);
        FAIL("agent did not complete within timeout");
    }

    if (mock_server_request_count() < 1) {
        shutdown_request();
        pthread_join(dt, NULL);
        db_close(db); chdir(orig_cwd);
        FAIL("mock server received no requests");
    }

    /* Verify inbox consumed + assistant entry in agent DB */
    adb = db_open_agent(adb_path);
    if (inbox_count(adb, sid) != 0) {
        db_close(adb); shutdown_request(); pthread_join(dt, NULL);
        db_close(db); chdir(orig_cwd);
        FAIL("inbox not consumed");
    }
    int count = 0;
    Entry *entries = session_get_branch(adb, sid, &count);
    int found_assistant = 0;
    if (entries) {
        for (int i = 0; i < count; i++) {
            if (entries[i].message.role == ROLE_ASSISTANT &&
                entries[i].message.content &&
                strstr(entries[i].message.content, "Hello from mock LLM"))
                found_assistant = 1;
        }
        entry_branch_free(entries, count);
    }
    db_close(adb);

    if (!found_assistant) {
        shutdown_request(); pthread_join(dt, NULL);
        db_close(db); chdir(orig_cwd);
        FAIL("assistant entry not found in DB");
    }

    shutdown_request();
    pthread_join(dt, NULL);
    db_close(db);
    unlink(DB_PATH);
    chdir(orig_cwd);
    PASS();
}

/* T130: daemon fork+reap with tool call — agent makes 2 LLM requests */
static void test_daemon_fork_reap_tool_call(void) {
    TEST(daemon_fork_reap_tool_call);

    mock_server_reset();

    mock_server_load("test/fixtures/daemon_fork_reap_tool_call.json");

    FILE *tf = fopen("/tmp/test_integ_daemon_file.txt", "w");
    if (tf) { fprintf(tf, "test data"); fclose(tf); }

    char orig_cwd[1024];
    getcwd(orig_cwd, sizeof(orig_cwd));
    setup_agent_dir();
    chdir(WORK_DIR);

    char adb_path[256];
    snprintf(adb_path, sizeof(adb_path), "agents/%s/agent.db", AGENT_NAME);
    sqlite3 *adb = db_open_agent(adb_path);
    if (!adb) { chdir(orig_cwd); FAIL("db_open_agent"); }
    int64_t sid = session_create(adb, "integ_daemon_tool", AGENT_NAME, -1, 0);
    if (sid < 0) { db_close(adb); chdir(orig_cwd); FAIL("session_create"); }
    inbox_insert(adb, sid, "test", "read the file");
    db_close(adb);

    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    if (!db) { chdir(orig_cwd); FAIL("db_open"); }
    seed_kv_config(db);
    db_kv_set(db, "provider.api_key", "mock-key");

    shutdown_reset();
    char self_path[4096];
    snprintf(self_path, sizeof(self_path), "%s/build/cclaw", orig_cwd);
    daemon_set_self_path(self_path);

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", s_port);
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

    daemon_signal_session_agent(sid, AGENT_NAME);

    if (wait_for_completion(adb_path, sid) != 0) {
        shutdown_request();
        pthread_join(dt, NULL);
        db_close(db); chdir(orig_cwd);
        FAIL("agent did not complete within timeout");
    }

    if (mock_server_request_count() < 2) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected >=2 requests, got %d",
                 mock_server_request_count());
        shutdown_request(); pthread_join(dt, NULL);
        db_close(db); chdir(orig_cwd);
        FAIL(msg);
    }

    adb = db_open_agent(adb_path);
    int count = 0;
    Entry *entries = session_get_branch(adb, sid, &count);
    int found = 0;
    if (entries) {
        for (int i = 0; i < count; i++) {
            if (entries[i].message.role == ROLE_ASSISTANT &&
                entries[i].message.content &&
                strstr(entries[i].message.content, "File contents received"))
                found = 1;
        }
        entry_branch_free(entries, count);
    }
    db_close(adb);

    if (!found) {
        shutdown_request(); pthread_join(dt, NULL);
        db_close(db); chdir(orig_cwd);
        FAIL("final assistant entry not found");
    }

    shutdown_request();
    pthread_join(dt, NULL);
    db_close(db);
    unlink(DB_PATH);
    unlink("/tmp/test_integ_daemon_file.txt");
    chdir(orig_cwd);
    PASS();
}

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    s_port = mock_server_start();
    printf("--- test_integration_daemon (T130) ---\n");
    test_daemon_fork_reap_mock();
    test_daemon_fork_reap_tool_call();
    printf("%d/%d passed\n", tests_passed, tests_run);
    curl_global_cleanup();
    mock_server_stop();
    return tests_passed == tests_run ? 0 : 1;
}
