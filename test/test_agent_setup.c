/* Test agent_setup — verify CLI mode excludes daemon tools,
 * daemon mode includes all tools. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "agent_setup.h"
#include "db.h"
#include "test_util.h"
#include "config.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

static void test_cli_mode_includes_agent_tools(void) {
    TEST(cli_mode_includes_agent_tools);
    const char *dbpath = "/tmp/test_agent_setup_cli.db";
    test_db_clean(dbpath);
    sqlite3 *db = test_db_open(dbpath);
    if (!db) { FAIL("db_open_agent"); return; }

    /* Create a session */
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    if (sid < 0) { FAIL("session_create"); db_close(db); test_db_clean(dbpath); return; }

    Config cfg = {0};
    cfg.workspace = "/tmp/test_ws";
    cfg.shell_timeout = 30;
    cfg.provider.api_key = "test";
    cfg.provider.base_url = "http://localhost";
    cfg.provider.model = "test";

    AgentSetup setup;
    agent_setup_init(&setup, db, sid, &cfg, "test_agent", AGENT_SETUP_CLI);

    /* launch_agent/check_session are now available in CLI too: the CLI runs the
     * same wake-pipe event loop as the daemon, so sub-agents advance there. A
     * depth-0 session is below the spawn ceiling, so launch_agent is present. */
    if (tools_lookup(&setup.reg, "launch_agent") == NULL) {
        FAIL("launch_agent should be in CLI mode"); agent_setup_destroy(&setup); db_close(db); test_db_clean(dbpath); return;
    }
    if (tools_lookup(&setup.reg, "check_session") == NULL) {
        FAIL("check_session should be in CLI mode"); agent_setup_destroy(&setup); db_close(db); test_db_clean(dbpath); return;
    }
    if (tools_lookup(&setup.reg, "create_agent") == NULL) {
        FAIL("create_agent should be available in CLI mode"); agent_setup_destroy(&setup); db_close(db); test_db_clean(dbpath); return;
    }
    if (tools_lookup(&setup.reg, "search_config") == NULL) {
        FAIL("search_config should be available in CLI mode"); agent_setup_destroy(&setup); db_close(db); test_db_clean(dbpath); return;
    }

    /* CLI should have core tools */
    if (tools_lookup(&setup.reg, "shell_exec") == NULL) {
        FAIL("shell_exec missing"); agent_setup_destroy(&setup); db_close(db); test_db_clean(dbpath); return;
    }
    if (tools_lookup(&setup.reg, "file_read") == NULL) {
        FAIL("file_read missing"); agent_setup_destroy(&setup); db_close(db); test_db_clean(dbpath); return;
    }
    if (tools_lookup(&setup.reg, "file_write") == NULL) {
        FAIL("file_write missing"); agent_setup_destroy(&setup); db_close(db); test_db_clean(dbpath); return;
    }
    if (tools_lookup(&setup.reg, "js_eval") == NULL) {
        FAIL("js_eval missing"); agent_setup_destroy(&setup); db_close(db); test_db_clean(dbpath); return;
    }
    if (tools_lookup(&setup.reg, "web_fetch") == NULL) {
        FAIL("web_fetch missing"); agent_setup_destroy(&setup); db_close(db); test_db_clean(dbpath); return;
    }
    if (tools_lookup(&setup.reg, "db_query") == NULL) {
        FAIL("db_query missing"); agent_setup_destroy(&setup); db_close(db); test_db_clean(dbpath); return;
    }
    if (tools_lookup(&setup.reg, "memory_create") == NULL) {
        FAIL("memory_create missing"); agent_setup_destroy(&setup); db_close(db); test_db_clean(dbpath); return;
    }

    agent_setup_destroy(&setup);
    db_close(db);
    test_db_clean(dbpath);
    PASS();
}

static void test_daemon_mode_includes_all_tools(void) {
    TEST(daemon_mode_includes_all_tools);
    const char *dbpath = "/tmp/test_agent_setup_daemon.db";
    test_db_clean(dbpath);
    sqlite3 *db = test_db_open(dbpath);
    if (!db) { FAIL("db_open_agent"); return; }

    int64_t sid = session_create(db, "test", NULL, -1, 0);
    if (sid < 0) { FAIL("session_create"); db_close(db); test_db_clean(dbpath); return; }

    Config cfg = {0};
    cfg.workspace = "/tmp/test_ws";
    cfg.shell_timeout = 30;
    cfg.provider.api_key = "test";
    cfg.provider.base_url = "http://localhost";
    cfg.provider.model = "test";

    AgentSetup setup;
    agent_setup_init(&setup, db, sid, &cfg, "test_agent", AGENT_SETUP_DAEMON);

    /* Daemon mode should have all tools including daemon-dependent ones */
    if (tools_lookup(&setup.reg, "launch_agent") == NULL) {
        FAIL("launch_agent missing in daemon mode"); agent_setup_destroy(&setup); db_close(db); test_db_clean(dbpath); return;
    }
    if (tools_lookup(&setup.reg, "create_agent") == NULL) {
        FAIL("create_agent missing in daemon mode"); agent_setup_destroy(&setup); db_close(db); test_db_clean(dbpath); return;
    }
    /* Plus core tools */
    if (tools_lookup(&setup.reg, "shell_exec") == NULL) {
        FAIL("shell_exec missing"); agent_setup_destroy(&setup); db_close(db); test_db_clean(dbpath); return;
    }
    if (tools_lookup(&setup.reg, "file_read") == NULL) {
        FAIL("file_read missing"); agent_setup_destroy(&setup); db_close(db); test_db_clean(dbpath); return;
    }
    if (tools_lookup(&setup.reg, "db_query") == NULL) {
        FAIL("db_query missing"); agent_setup_destroy(&setup); db_close(db); test_db_clean(dbpath); return;
    }

    agent_setup_destroy(&setup);
    db_close(db);
    test_db_clean(dbpath);
    PASS();
}

int main(void) {
    TEST_INIT();
    printf("test_agent_setup:\n");
    test_cli_mode_includes_agent_tools();
    test_daemon_mode_includes_all_tools();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
