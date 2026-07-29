/* Test agent_setup — verify the expected tools are registered. Tool
 * availability is mode-independent (decided by session depth and grants),
 * so one registration check covers both CLI and daemon. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include "agent_setup.h"
#include "db.h"
#include "test_util.h"
#include "config.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

static void test_all_tools_registered(void) {
    TEST(all_tools_registered);
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
    agent_setup_init(&setup, db, sid, &cfg, "test_agent");

    /* launch_agent/check_session run the same wake-pipe event loop in CLI and
     * daemon. A depth-0 session is below the spawn ceiling, so launch_agent is
     * present. */
    if (tools_lookup(&setup.reg, "launch_agent") == NULL) {
        FAIL("launch_agent missing"); agent_setup_destroy(&setup); db_close(db); test_db_clean(dbpath); return;
    }
    if (tools_lookup(&setup.reg, "check_session") == NULL) {
        FAIL("check_session missing"); agent_setup_destroy(&setup); db_close(db); test_db_clean(dbpath); return;
    }
    if (tools_lookup(&setup.reg, "create_agent") == NULL) {
        FAIL("create_agent missing"); agent_setup_destroy(&setup); db_close(db); test_db_clean(dbpath); return;
    }
    if (tools_lookup(&setup.reg, "search_config") == NULL) {
        FAIL("search_config missing"); agent_setup_destroy(&setup); db_close(db); test_db_clean(dbpath); return;
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

/* One AgentSetup serves every agent in the daemon, so refresh_caps must move
 * the tool contexts to the advancing agent's own workspace — unless the
 * operator pinned one. */
static void test_workspace_follows_agent(void) {
    TEST(workspace_follows_agent);
    const char *root = "/tmp/cclaw_test_ws_root";
    const char *dbpath = "/tmp/cclaw_test_ws_root/cclaw.db";
    system("rm -rf /tmp/cclaw_test_ws_root && mkdir -p /tmp/cclaw_test_ws_root");
    test_db_clean(dbpath);
    sqlite3 *db = test_db_open(dbpath);
    if (!db) { FAIL("db_open"); return; }

    int64_t sid = session_create(db, "Alpha", NULL, -1, 0);
    if (sid < 0) { FAIL("session_create"); db_close(db); test_db_clean(dbpath); return; }

    Config cfg = {0};
    cfg.db_path = (char *)dbpath;
    cfg.workspace = "/tmp/cclaw_test_ws_root/pinned";
    cfg.shell_timeout = 30;

    AgentSetup setup;
    agent_setup_init(&setup, db, sid, &cfg, "Alpha");

    char want[512];
    snprintf(want, sizeof(want), "%s/agents/Alpha/workspace", root);
    if (!setup.file_read_ctx.workspace ||
        strcmp(setup.file_read_ctx.workspace, want) != 0) {
        FAIL("Alpha workspace not derived");
        agent_setup_destroy(&setup); db_close(db); test_db_clean(dbpath); return;
    }

    agent_setup_refresh_caps(&setup, db, "Beta");
    snprintf(want, sizeof(want), "%s/agents/Beta/workspace", root);
    if (strcmp(setup.file_read_ctx.workspace, want) != 0 ||
        strcmp(setup.js_eval_ctx.workspace, want) != 0 ||
        strcmp(setup.web_ctx.workspace, want) != 0 ||
        strcmp(setup.ext_tool_ctx.workspace, want) != 0) {
        FAIL("Beta workspace not rebound");
        agent_setup_destroy(&setup); db_close(db); test_db_clean(dbpath); return;
    }
    ToolEntry *sh = tools_lookup(&setup.reg, "shell_exec");
    if (!sh || !sh->user_data ||
        strcmp(((ShellConfig *)sh->user_data)->workspace, want) != 0) {
        FAIL("shell workspace not rebound");
        agent_setup_destroy(&setup); db_close(db); test_db_clean(dbpath); return;
    }
    agent_setup_destroy(&setup);

    /* Explicit workspace wins: no derivation, every agent shares it. */
    cfg.workspace_explicit = 1;
    agent_setup_init(&setup, db, sid, &cfg, "Alpha");
    if (strcmp(setup.file_read_ctx.workspace, cfg.workspace) != 0) {
        FAIL("explicit workspace overridden");
        agent_setup_destroy(&setup); db_close(db); test_db_clean(dbpath); return;
    }
    agent_setup_refresh_caps(&setup, db, "Beta");
    if (strcmp(setup.file_read_ctx.workspace, cfg.workspace) != 0 ||
        strcmp(setup.ext_tool_ctx.workspace, cfg.workspace) != 0) {
        FAIL("explicit workspace overridden on refresh");
        agent_setup_destroy(&setup); db_close(db); test_db_clean(dbpath); return;
    }

    agent_setup_destroy(&setup);
    db_close(db);
    test_db_clean(dbpath);
    system("rm -rf /tmp/cclaw_test_ws_root");
    PASS();
}

int main(void) {
    TEST_INIT();
    printf("test_agent_setup:\n");
    test_all_tools_registered();
    test_workspace_follows_agent();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
