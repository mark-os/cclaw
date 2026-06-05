/* T130/T298: integration test — daemon fork+reap with mock LLM.
 * Daemon forks agent process, agent hits mock HTTP server, writes response,
 * daemon reaps and delivers. Verifies V21 (daemon forks), V26 (delivers).
 * T298: Updated to single unified DB (V73/T297). */
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

/* T298: Setup workspace dir for agent (needed for workspace isolation) */
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
static int wait_for_completion(sqlite3 *db, int64_t sid) {
    for (int i = 0; i < 150; i++) {
        usleep(100000);
        sqlite3_stmt *stmt;
        int done = 0;
        sqlite3_prepare_v2(db,
            "SELECT state FROM sessions WHERE id=?;", -1, &stmt, NULL);
        sqlite3_bind_int64(stmt, 1, sid);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *state = (const char *)sqlite3_column_text(stmt, 0);
            if (state && strcmp(state, "idle") == 0) {
                sqlite3_finalize(stmt);
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

    mock_server_reset();

    mock_server_enqueue(200,
        "{\"id\":\"chatcmpl-d1\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"Hello from mock LLM\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}");

    char orig_cwd[1024];
    getcwd(orig_cwd, sizeof(orig_cwd));
    setup_agent_dir();
    chdir(WORK_DIR);

    /* T298: Single unified DB for daemon + agent */
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    if (!db) { chdir(orig_cwd); FAIL("db_open"); }
    seed_kv_config(db);
    db_kv_set(db, "provider.api_key", "mock-key");

    /* Create session + inbox in same DB */
    int64_t sid = session_create(db, "integ_daemon", AGENT_NAME, -1, 0);
    if (sid < 0) { db_close(db); chdir(orig_cwd); FAIL("session_create"); }
    inbox_insert(db, sid, "test", "hi there");

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

    if (wait_for_completion(db, sid) != 0) {
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

    /* Verify inbox consumed + assistant entry */
    if (inbox_count(db, sid) != 0) {
        shutdown_request(); pthread_join(dt, NULL);
        db_close(db); chdir(orig_cwd);
        FAIL("inbox not consumed");
    }
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
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

    if (!found_assistant) {
        shutdown_request(); pthread_join(dt, NULL);
        db_close(db); chdir(orig_cwd);
        FAIL("assistant entry not found in DB");
    }

    /* T246: Verify channel_outbox row written (last_route="test", not "cli") */
    {
        sqlite3_stmt *ostmt;
        int rc = sqlite3_prepare_v2(db,
            "SELECT channel_name, payload FROM channel_outbox"
            " WHERE session_id=? AND status='pending';", -1, &ostmt, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(ostmt, 1, sid);
            if (sqlite3_step(ostmt) == SQLITE_ROW) {
                const char *ch = (const char *)sqlite3_column_text(ostmt, 0);
                const char *pl = (const char *)sqlite3_column_text(ostmt, 1);
                if (!ch || strcmp(ch, "test") != 0) {
                    sqlite3_finalize(ostmt);
                    shutdown_request(); pthread_join(dt, NULL);
                    db_close(db); chdir(orig_cwd);
                    FAIL("channel_outbox channel_name != 'test'");
                }
                if (!pl || !strstr(pl, "Hello from mock LLM")) {
                    sqlite3_finalize(ostmt);
                    shutdown_request(); pthread_join(dt, NULL);
                    db_close(db); chdir(orig_cwd);
                    FAIL("channel_outbox payload missing response");
                }
            } else {
                sqlite3_finalize(ostmt);
                shutdown_request(); pthread_join(dt, NULL);
                db_close(db); chdir(orig_cwd);
                FAIL("no channel_outbox row found");
            }
            sqlite3_finalize(ostmt);
        }
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

    /* T298: Single unified DB */
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    if (!db) { chdir(orig_cwd); FAIL("db_open"); }
    seed_kv_config(db);
    db_kv_set(db, "provider.api_key", "mock-key");

    /* Create session + inbox in same DB */
    int64_t sid = session_create(db, "integ_daemon_tool", AGENT_NAME, -1, 0);
    if (sid < 0) { db_close(db); chdir(orig_cwd); FAIL("session_create"); }
    inbox_insert(db, sid, "test", "read the file");

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

    if (wait_for_completion(db, sid) != 0) {
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

    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
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
    printf("--- test_integration_daemon (T130/T298) ---\n");
    test_daemon_fork_reap_mock();
    test_daemon_fork_reap_tool_call();
    printf("%d/%d passed\n", tests_passed, tests_run);
    curl_global_cleanup();
    mock_server_stop();
    return tests_passed == tests_run ? 0 : 1;
}
