/* T207: Integration tests for 3-DB architecture (V73).
 * Tests cross-DB flows (daemon↔agent DB), restart recovery, CLI standalone.
 */
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

static const char *WORK_DIR = "/tmp/test_integ_3db_work";
static const char *DAEMON_DB = "/tmp/test_integ_3db_daemon.db";

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

static void cleanup_work_dir(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", WORK_DIR);
    system(cmd);
}

static void setup_work_dir(const char *agent_name) {
    cleanup_work_dir();
    mkdir(WORK_DIR, 0755);
    char buf[512];
    snprintf(buf, sizeof(buf), "%s/agents", WORK_DIR);
    mkdir(buf, 0755);
    snprintf(buf, sizeof(buf), "%s/agents/%s", WORK_DIR, agent_name);
    mkdir(buf, 0755);
    snprintf(buf, sizeof(buf), "%s/agents/%s/workspace", WORK_DIR, agent_name);
    mkdir(buf, 0755);
}

/* T207/V73: daemon_inbox_insert writes to agent DB, agent reads it */
static void test_cross_db_inbox_write(void) {
    TEST(cross_db_inbox_write);
    const char *agent = "crossdb_agent";
    setup_work_dir(agent);

    char orig_cwd[1024];
    getcwd(orig_cwd, sizeof(orig_cwd));
    chdir(WORK_DIR);

    /* Create agent DB with a session */
    char adb_path[256];
    snprintf(adb_path, sizeof(adb_path), "agents/%s/agent.db", agent);
    sqlite3 *adb = db_open_agent(adb_path);
    if (!adb) { chdir(orig_cwd); FAIL("db_open_agent"); }
    int64_t sid = session_create(adb, "test", agent, -1, 0);
    if (sid < 0) { db_close(adb); chdir(orig_cwd); FAIL("session_create"); }
    db_close(adb);

    /* Daemon writes inbox to agent DB via daemon_inbox_insert */
    int64_t iid = daemon_inbox_insert(agent, sid, "telegram", "hello from daemon");
    if (iid < 0) { chdir(orig_cwd); FAIL("daemon_inbox_insert returned -1"); }

    /* Verify: open agent DB, check inbox has the message */
    adb = db_open_agent(adb_path);
    if (!adb) { chdir(orig_cwd); FAIL("reopen agent db"); }
    int count = inbox_count(adb, sid);
    if (count != 1) { db_close(adb); chdir(orig_cwd); FAIL("inbox_count != 1"); }

    int peek_count = 0;
    InboxItem *items = inbox_peek(adb, sid, 10, &peek_count);
    if (peek_count != 1 || !items) {
        inbox_items_free(items, peek_count);
        db_close(adb); chdir(orig_cwd); FAIL("inbox_peek failed");
    }
    if (strcmp(items[0].source, "telegram") != 0 ||
        strcmp(items[0].payload, "hello from daemon") != 0) {
        inbox_items_free(items, peek_count);
        db_close(adb); chdir(orig_cwd); FAIL("inbox content mismatch");
    }
    inbox_items_free(items, peek_count);
    db_close(adb);
    chdir(orig_cwd);
    PASS();
}

/* T207/V73: cclaw.db and agent.db have separate schemas — no cross-contamination */
static void test_schema_isolation(void) {
    TEST(schema_isolation);
    unlink(DAEMON_DB);

    sqlite3 *ddb = db_open_cclaw(DAEMON_DB);
    if (!ddb) FAIL("db_open_cclaw");

    /* cclaw.db should NOT have sessions/entries tables */
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(ddb,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name='sessions';",
        -1, &stmt, NULL);
    assert(rc == SQLITE_OK);
    int has_sessions = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    db_close(ddb);
    unlink(DAEMON_DB);

    if (has_sessions) FAIL("cclaw.db should not have sessions table");

    /* agent.db should NOT have agents/agent_config tables */
    const char *agent_path = "/tmp/test_integ_3db_agent_iso.db";
    unlink(agent_path);
    sqlite3 *adb = db_open_agent(agent_path);
    if (!adb) FAIL("db_open_agent");

    rc = sqlite3_prepare_v2(adb,
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name='agent_config';",
        -1, &stmt, NULL);
    assert(rc == SQLITE_OK);
    int has_agent_config = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    db_close(adb);
    unlink(agent_path);

    if (has_agent_config) FAIL("agent.db should not have agent_config table");
    PASS();
}

/* T207/V34: Restart recovery resets running sessions to idle */
static void test_restart_recovery(void) {
    TEST(restart_recovery);
    const char *agent = "recovery_agent";
    setup_work_dir(agent);

    char orig_cwd[1024];
    getcwd(orig_cwd, sizeof(orig_cwd));
    chdir(WORK_DIR);

    /* Create agent DB with a session stuck in "running" state */
    char adb_path[256];
    snprintf(adb_path, sizeof(adb_path), "agents/%s/agent.db", agent);
    sqlite3 *adb = db_open_agent(adb_path);
    if (!adb) { chdir(orig_cwd); FAIL("db_open_agent"); }
    int64_t sid = session_create(adb, "stuck", agent, -1, 0);
    session_set_state(adb, sid, "running");
    db_close(adb);

    /* Create daemon DB */
    unlink(DAEMON_DB);
    sqlite3 *ddb = db_open_cclaw(DAEMON_DB);
    if (!ddb) { chdir(orig_cwd); FAIL("db_open_cclaw"); }
    db_close(ddb);

    /* Run startup recovery */
    ddb = db_open_cclaw(DAEMON_DB);
    daemon_startup_recovery(ddb);
    db_close(ddb);

    /* Verify session is now idle */
    adb = db_open_agent(adb_path);
    if (!adb) { chdir(orig_cwd); FAIL("reopen agent db"); }
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(adb, "SELECT state FROM sessions WHERE id=?;", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, sid);
    const char *state = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        state = (const char *)sqlite3_column_text(stmt, 0);
    int is_idle = (state && strcmp(state, "idle") == 0);
    sqlite3_finalize(stmt);
    db_close(adb);
    chdir(orig_cwd);

    if (!is_idle) FAIL("session not reset to idle after recovery");
    PASS();
}

/* T207/V81: CLI standalone opens agent DB directly, no cclaw.db needed */
static void test_cli_standalone_no_daemon_db(void) {
    TEST(cli_standalone_no_daemon_db);
    const char *agent = "cli_agent";
    setup_work_dir(agent);

    char orig_cwd[1024];
    getcwd(orig_cwd, sizeof(orig_cwd));
    chdir(WORK_DIR);

    /* Create agent DB */
    char adb_path[512];
    snprintf(adb_path, sizeof(adb_path), "%s/agents/%s/agent.db", WORK_DIR, agent);
    sqlite3 *adb = db_open_agent(adb_path);
    if (!adb) { chdir(orig_cwd); FAIL("db_open_agent"); }

    /* Can create session + append entries without cclaw.db */
    int64_t sid = session_create(adb, "cli_test", agent, -1, 0);
    if (sid < 0) { db_close(adb); chdir(orig_cwd); FAIL("session_create"); }

    Message msg = {.role = ROLE_USER, .content = "hello from CLI"};
    int64_t eid = entry_append(adb, sid, &msg);
    if (eid < 0) { db_close(adb); chdir(orig_cwd); FAIL("entry_append"); }

    /* Can set up agent tools in CLI mode (no daemon tools) */
    Config cfg = {0};
    cfg.workspace = "/tmp";
    cfg.shell_timeout = 30;
    cfg.provider.api_key = "test";
    cfg.provider.base_url = "http://localhost";
    cfg.provider.model = "test";

    AgentSetup setup;
    int rc = agent_setup_init(&setup, adb, sid, &cfg, agent, NULL, 0, AGENT_SETUP_CLI);
    if (rc != 0) { db_close(adb); chdir(orig_cwd); FAIL("agent_setup_init"); }

    /* Verify core tools present, daemon tools absent */
    if (!tools_lookup(&setup.reg, "shell_exec")) {
        agent_setup_destroy(&setup); db_close(adb); chdir(orig_cwd);
        FAIL("shell_exec missing in CLI mode");
    }
    if (tools_lookup(&setup.reg, "launch_agent")) {
        agent_setup_destroy(&setup); db_close(adb); chdir(orig_cwd);
        FAIL("launch_agent should not be in CLI mode");
    }

    agent_setup_destroy(&setup);
    db_close(adb);
    chdir(orig_cwd);
    PASS();
}

/* T207/V73: daemon_inbox_insert + agent consume → entries flow */
static void test_cross_db_inbox_to_entries(void) {
    TEST(cross_db_inbox_to_entries);
    const char *agent = "flow_agent";
    setup_work_dir(agent);

    char orig_cwd[1024];
    getcwd(orig_cwd, sizeof(orig_cwd));
    chdir(WORK_DIR);

    char adb_path[256];
    snprintf(adb_path, sizeof(adb_path), "agents/%s/agent.db", agent);
    sqlite3 *adb = db_open_agent(adb_path);
    if (!adb) { chdir(orig_cwd); FAIL("db_open_agent"); }
    int64_t sid = session_create(adb, "flow", agent, -1, 0);
    db_close(adb);

    /* Daemon writes multiple inbox items */
    daemon_inbox_insert(agent, sid, "telegram", "msg1");
    daemon_inbox_insert(agent, sid, "telegram", "msg2");

    /* Agent opens own DB, consumes inbox into entries */
    adb = db_open_agent(adb_path);
    if (!adb) { chdir(orig_cwd); FAIL("reopen agent db"); }
    int consumed = inbox_consume_into_entries(adb, sid, 10);
    if (consumed != 2) { db_close(adb); chdir(orig_cwd); FAIL("expected 2 consumed"); }

    /* Verify entries exist */
    int count = 0;
    Entry *entries = session_get_branch(adb, sid, &count);
    if (count != 2) {
        entry_branch_free(entries, count);
        db_close(adb); chdir(orig_cwd); FAIL("expected 2 entries");
    }
    int found_msg1 = 0, found_msg2 = 0;
    for (int i = 0; i < count; i++) {
        if (entries[i].message.content) {
            if (strstr(entries[i].message.content, "msg1")) found_msg1 = 1;
            if (strstr(entries[i].message.content, "msg2")) found_msg2 = 1;
        }
    }
    entry_branch_free(entries, count);
    db_close(adb);
    chdir(orig_cwd);

    if (!found_msg1 || !found_msg2) FAIL("messages not found in entries");
    PASS();
}

/* T207/V34: Restart recovery handles "waiting" state — resets to idle + injects recovery msg */
static void test_restart_recovery_waiting(void) {
    TEST(restart_recovery_waiting);
    const char *agent = "wait_agent";
    setup_work_dir(agent);

    char orig_cwd[1024];
    getcwd(orig_cwd, sizeof(orig_cwd));
    chdir(WORK_DIR);

    char adb_path[256];
    snprintf(adb_path, sizeof(adb_path), "agents/%s/agent.db", agent);
    sqlite3 *adb = db_open_agent(adb_path);
    if (!adb) { chdir(orig_cwd); FAIL("db_open_agent"); }

    int64_t sid = session_create(adb, "waiting", agent, -1, 0);
    /* State machine: idle → running → waiting */
    session_set_state(adb, sid, "running");
    session_set_state(adb, sid, "waiting");
    db_close(adb);

    /* Run startup recovery */
    unlink(DAEMON_DB);
    sqlite3 *ddb = db_open_cclaw(DAEMON_DB);
    daemon_startup_recovery(ddb);
    db_close(ddb);

    /* Verify session reset to idle + recovery inbox message inserted */
    adb = db_open_agent(adb_path);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(adb, "SELECT state FROM sessions WHERE id=?;", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, sid);
    const char *state = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        state = (const char *)sqlite3_column_text(stmt, 0);
    int is_idle = (state && strcmp(state, "idle") == 0);
    sqlite3_finalize(stmt);

    /* Recovery should have inserted an inbox message */
    int icount = inbox_count(adb, sid);
    db_close(adb);
    chdir(orig_cwd);

    if (!is_idle) FAIL("waiting session should be reset to idle on recovery");
    if (icount < 1) FAIL("recovery should insert inbox message for waiting session");
    PASS();
}

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    printf("--- test_integration_3db (T207/V73) ---\n");
    test_cross_db_inbox_write();
    test_schema_isolation();
    test_restart_recovery();
    test_cli_standalone_no_daemon_db();
    test_cross_db_inbox_to_entries();
    test_restart_recovery_waiting();
    printf("%d/%d passed\n", tests_passed, tests_run);
    cleanup_work_dir();
    unlink(DAEMON_DB);
    curl_global_cleanup();
    return tests_passed == tests_run ? 0 : 1;
}
