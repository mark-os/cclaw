/* T298: Integration tests for single-DB architecture (V73/T297).
 * Tests daemon helpers, restart recovery, CLI standalone — all on one cclaw.db. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include <curl/curl.h>
#include "daemon.h"
#include "db.h"
#include "config.h"
#include "agent_setup.h"
#include "shutdown.h"
#include "mock_server.h"

static const char *DB_PATH = "/tmp/test_integ_3db.db";

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

static void cleanup(void) {
    unlink(DB_PATH);
    char buf[256];
    snprintf(buf, sizeof(buf), "%s-wal", DB_PATH);
    unlink(buf);
    snprintf(buf, sizeof(buf), "%s-shm", DB_PATH);
    unlink(buf);
}

/* T298/V73: daemon_inbox_insert writes to unified DB when daemon_set_db_path set */
static void test_inbox_write_unified(void) {
    TEST(cross_db_inbox_write);
    cleanup();

    sqlite3 *db = db_open(DB_PATH);
    if (!db) FAIL("db_open");
    int64_t sid = session_create(db, "test", "testagent", -1, 0);
    if (sid < 0) { db_close(db); FAIL("session_create"); }
    db_close(db);

    daemon_set_db_path(DB_PATH);
    int64_t iid = daemon_inbox_insert("testagent", sid, "telegram", "hello from daemon");
    if (iid < 0) FAIL("daemon_inbox_insert returned -1");

    db = db_open(DB_PATH);
    if (!db) FAIL("reopen db");
    int count = inbox_count(db, sid);
    if (count != 1) { db_close(db); FAIL("inbox_count != 1"); }

    int peek_count = 0;
    InboxItem *items = inbox_peek(db, sid, 10, &peek_count);
    if (peek_count != 1 || !items) {
        inbox_items_free(items, peek_count);
        db_close(db); FAIL("inbox_peek failed");
    }
    if (strcmp(items[0].source, "telegram") != 0 ||
        strcmp(items[0].payload, "hello from daemon") != 0) {
        inbox_items_free(items, peek_count);
        db_close(db); FAIL("inbox content mismatch");
    }
    inbox_items_free(items, peek_count);
    db_close(db);
    PASS();
}

/* T298/V73: unified DB has all tables — sessions, entries, agents, kv, channels */
static void test_unified_schema(void) {
    TEST(schema_isolation);
    cleanup();

    sqlite3 *db = db_open(DB_PATH);
    if (!db) FAIL("db_open");

    /* Verify sessions table exists */
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name='sessions';",
        -1, &stmt, NULL);
    assert(rc == SQLITE_OK);
    int has_sessions = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    if (!has_sessions) { db_close(db); FAIL("unified DB missing sessions table"); }

    /* Verify kv table exists */
    rc = sqlite3_prepare_v2(db,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name='kv';",
        -1, &stmt, NULL);
    assert(rc == SQLITE_OK);
    int has_kv = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    if (!has_kv) { db_close(db); FAIL("unified DB missing kv table"); }

    /* Verify channels table exists */
    rc = sqlite3_prepare_v2(db,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name='channels';",
        -1, &stmt, NULL);
    assert(rc == SQLITE_OK);
    int has_channels = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    if (!has_channels) { db_close(db); FAIL("unified DB missing channels table"); }

    db_close(db);
    PASS();
}

/* T298/V34: Restart recovery resets running sessions to idle */
static void test_restart_recovery(void) {
    TEST(restart_recovery);
    cleanup();

    sqlite3 *db = db_open(DB_PATH);
    if (!db) FAIL("db_open");
    int64_t sid = session_create(db, "stuck", "recovery_agent", -1, 0);
    session_set_state(db, sid, "running");

    daemon_startup_recovery(db);

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT state FROM sessions WHERE id=?;", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, sid);
    const char *state = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        state = (const char *)sqlite3_column_text(stmt, 0);
    int is_idle = (state && strcmp(state, "idle") == 0);
    sqlite3_finalize(stmt);
    db_close(db);

    if (!is_idle) FAIL("session not reset to idle after recovery");
    PASS();
}

/* T298/V81: CLI standalone opens unified DB directly */
static void test_cli_standalone(void) {
    TEST(cli_standalone_no_daemon_db);
    cleanup();

    sqlite3 *db = db_open(DB_PATH);
    if (!db) FAIL("db_open");

    int64_t sid = session_create(db, "cli_test", "cli_agent", -1, 0);
    if (sid < 0) { db_close(db); FAIL("session_create"); }

    Message msg = {.role = ROLE_USER, .content = "hello from CLI"};
    int64_t eid = entry_append(db, sid, &msg);
    if (eid < 0) { db_close(db); FAIL("entry_append"); }

    Config cfg = {0};
    cfg.workspace = "/tmp";
    cfg.shell_timeout = 30;
    cfg.provider.api_key = "test";
    cfg.provider.base_url = "http://localhost";
    cfg.provider.model = "test";

    AgentSetup setup;
    int rc = agent_setup_init(&setup, db, sid, &cfg, "cli_agent", NULL, 0, AGENT_SETUP_CLI);
    if (rc != 0) { db_close(db); FAIL("agent_setup_init"); }

    if (!tools_lookup(&setup.reg, "shell_exec")) {
        agent_setup_destroy(&setup); db_close(db);
        FAIL("shell_exec missing in CLI mode");
    }
    if (tools_lookup(&setup.reg, "launch_agent")) {
        agent_setup_destroy(&setup); db_close(db);
        FAIL("launch_agent should not be in CLI mode");
    }

    agent_setup_destroy(&setup);
    db_close(db);
    PASS();
}

/* T298/V73: inbox_insert + consume → entries flow in unified DB */
static void test_inbox_to_entries(void) {
    TEST(cross_db_inbox_to_entries);
    cleanup();

    sqlite3 *db = db_open(DB_PATH);
    if (!db) FAIL("db_open");
    int64_t sid = session_create(db, "flow", "flow_agent", -1, 0);

    daemon_set_db_path(DB_PATH);
    daemon_inbox_insert("flow_agent", sid, "telegram", "msg1");
    daemon_inbox_insert("flow_agent", sid, "telegram", "msg2");

    int consumed = inbox_consume_into_entries(db, sid, 10);
    if (consumed != 2) { db_close(db); FAIL("expected 2 consumed"); }

    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    if (count != 2) {
        entry_branch_free(entries, count);
        db_close(db); FAIL("expected 2 entries");
    }
    int found_msg1 = 0, found_msg2 = 0;
    for (int i = 0; i < count; i++) {
        if (entries[i].message.content) {
            if (strstr(entries[i].message.content, "msg1")) found_msg1 = 1;
            if (strstr(entries[i].message.content, "msg2")) found_msg2 = 1;
        }
    }
    entry_branch_free(entries, count);
    db_close(db);

    if (!found_msg1 || !found_msg2) FAIL("messages not found in entries");
    PASS();
}

/* T298/V34: Restart recovery handles "waiting" state */
static void test_restart_recovery_waiting(void) {
    TEST(restart_recovery_waiting);
    cleanup();

    sqlite3 *db = db_open(DB_PATH);
    if (!db) FAIL("db_open");
    int64_t sid = session_create(db, "waiting", "wait_agent", -1, 0);
    session_set_state(db, sid, "running");
    session_set_state(db, sid, "waiting");

    daemon_startup_recovery(db);

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT state FROM sessions WHERE id=?;", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, sid);
    const char *state = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        state = (const char *)sqlite3_column_text(stmt, 0);
    int is_idle = (state && strcmp(state, "idle") == 0);
    sqlite3_finalize(stmt);

    int icount = inbox_count(db, sid);
    db_close(db);

    if (!is_idle) FAIL("waiting session should be reset to idle on recovery");
    if (icount < 1) FAIL("recovery should insert inbox message for waiting session");
    PASS();
}

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    printf("--- test_integration_3db (T298/V73) ---\n");
    test_inbox_write_unified();
    test_unified_schema();
    test_restart_recovery();
    test_cli_standalone();
    test_inbox_to_entries();
    test_restart_recovery_waiting();
    printf("%d/%d passed\n", tests_passed, tests_run);
    cleanup();
    curl_global_cleanup();
    return tests_passed == tests_run ? 0 : 1;
}
