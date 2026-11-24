/* Test agent_setup — verify the expected tools are registered. Tool
 * availability is mode-independent (decided by session depth and grants),
 * so one registration check covers both CLI and daemon. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include "agent_setup.h"
#include "db.h"
#include "test_util.h"
#include "config.h"

static void test_all_tools_registered(void) {
    const char *dbpath = "/tmp/test_agent_setup_cli.db";
    test_db_clean(dbpath);
    sqlite3 *db = test_db_open(dbpath);
    assert(db);

    /* Create a session */
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    assert(sid > 0);

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
    static const char *const want_tools[] = {
        "launch_agent", "check_session", "create_agent", "search_config",
        "shell_exec", "file_read", "file_write", "js_eval",
        "db_query", "memory_block_create" };
    for (size_t i = 0; i < sizeof(want_tools) / sizeof(want_tools[0]); i++) {
        if (!tools_lookup(&setup.reg, want_tools[i]))
            fprintf(stderr, "missing tool: %s\n", want_tools[i]);
        assert(tools_lookup(&setup.reg, want_tools[i]) != NULL);
    }

    /* The sync carried each recipe's droppable flag into the tools table,
     * where turn_edit.c reads it. This is the whole list of built-in tools
     * whose calls a hook may take out of context; every other one
     * — and any tool added without the flag — is protected. check_session is
     * not on it: on an idle child it returns the child's answer and advances
     * the delivery cursor, so its result may be the only copy. */
    char *droppable = db_scalar_text(db,
        "SELECT group_concat(name, ',' ORDER BY name) FROM tools"
        " WHERE droppable=1 AND extension_name IS NULL", 0);
    const char *want = "cron_list,db_query,extension_list,file_find,"
                       "file_grep,file_list,file_read,js_eval,search_config,"
                       "search_models,shell_exec";
    if (!droppable || strcmp(droppable, want) != 0)
        fprintf(stderr, "droppable tools: got %s\n", droppable ? droppable : "(null)");
    assert(droppable && strcmp(droppable, want) == 0);
    free(droppable);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM tools WHERE droppable=0 AND name IN"
                          " ('file_write','file_edit','channel_send','launch_agent',"
                          "  'memory_note_add','request_config','cron_set','secret_create',"
                          "  'check_session')"
                          , 0, -1) == 9);

    agent_setup_destroy(&setup);
    db_close(db);
    test_db_clean(dbpath);
}

/* One AgentSetup serves every agent in the daemon, so refresh_caps must move
 * the tool contexts to the advancing agent's own workspace — unless the
 * operator pinned one. */
static void test_workspace_follows_agent(void) {
    const char *root = "/tmp/cclaw_test_ws_root";
    const char *dbpath = "/tmp/cclaw_test_ws_root/cclaw.db";
    assert(system("rm -rf /tmp/cclaw_test_ws_root && mkdir -p /tmp/cclaw_test_ws_root") == 0);
    test_db_clean(dbpath);
    sqlite3 *db = test_db_open(dbpath);
    assert(db);

    int64_t sid = session_create(db, "Alpha", NULL, -1, 0);
    assert(sid > 0);

    Config cfg = {0};
    cfg.db_path = (char *)dbpath;
    cfg.workspace = "/tmp/cclaw_test_ws_root/pinned";
    cfg.shell_timeout = 30;

    AgentSetup setup;
    agent_setup_init(&setup, db, sid, &cfg, "Alpha");

    char want[512];
    snprintf(want, sizeof(want), "%s/agents/Alpha/workspace", root);
    assert(setup.file_read_ctx.workspace &&
           strcmp(setup.file_read_ctx.workspace, want) == 0);

    agent_setup_refresh_caps(&setup, db, "Beta");
    snprintf(want, sizeof(want), "%s/agents/Beta/workspace", root);
    assert(strcmp(setup.file_read_ctx.workspace, want) == 0);
    assert(strcmp(setup.js_eval_ctx.workspace, want) == 0);
    assert(strcmp(setup.ext_tool_ctx.workspace, want) == 0);
    ToolEntry *sh = tools_lookup(&setup.reg, "shell_exec");
    assert(sh && sh->user_data &&
           strcmp(((ShellConfig *)sh->user_data)->workspace, want) == 0);
    agent_setup_destroy(&setup);

    /* Explicit workspace wins: no derivation, every agent shares it. */
    cfg.workspace_explicit = 1;
    agent_setup_init(&setup, db, sid, &cfg, "Alpha");
    assert(strcmp(setup.file_read_ctx.workspace, cfg.workspace) == 0);
    agent_setup_refresh_caps(&setup, db, "Beta");
    assert(strcmp(setup.file_read_ctx.workspace, cfg.workspace) == 0);
    assert(strcmp(setup.ext_tool_ctx.workspace, cfg.workspace) == 0);

    agent_setup_destroy(&setup);
    db_close(db);
    test_db_clean(dbpath);
    assert(system("rm -rf /tmp/cclaw_test_ws_root") == 0);
}

int main(void) {
    TEST_INIT();
    printf("test_agent_setup:\n");
    RUN(test_all_tools_registered);
    RUN(test_workspace_follows_agent);
    printf("all agent_setup tests passed\n");
    return 0;
}
