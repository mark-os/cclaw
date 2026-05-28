#define _POSIX_C_SOURCE 200809L
#include "agent_exit.h"
#include "db.h"
#include "tool_approval.h"
#include "tool_bootstrap.h"
#include "tool_agent.h"
#include "tools.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sqlite3 *setup(void) {
    sqlite3 *db = db_open(":memory:");
    assert(db);
    return db;
}

/* T197: verify exit code constants */
static void test_exit_codes(void) {
    assert(AGENT_EXIT_DONE == 0);
    assert(AGENT_EXIT_ERROR == 1);
    assert(AGENT_EXIT_SPAWN == 2);
    assert(AGENT_EXIT_APPROVAL == 3);
    assert(AGENT_EXIT_CONFIG == 4);
    printf("  PASS: exit code constants\n");
}

/* T197: approval_request returns SENTINEL_APPROVAL prefix */
static void test_approval_sentinel(void) {
    sqlite3 *db = setup();
    ToolRegistry reg;
    tools_init(&reg);
    ToolApprovalCtx ctx = {.db = db, .session_id = 1, .agent_name = "test"};
    tool_approval_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "approval_request");
    assert(e);
    char *result = e->handler(
        "{\"type\":\"whitelist_host\",\"payload\":{\"host\":\"x.com\"}}",
        e->user_data);
    assert(result);
    assert(strncmp(result, SENTINEL_APPROVAL, strlen(SENTINEL_APPROVAL)) == 0);
    free(result);
    tools_free(&reg);
    db_close(db);
    printf("  PASS: approval_request returns SENTINEL_APPROVAL\n");
}

/* T197: configure_provider returns SENTINEL_CONFIG prefix */
static void test_configure_provider_sentinel(void) {
    sqlite3 *db = setup();
    ToolRegistry reg;
    tools_init(&reg);
    ToolBootstrapCtx ctx = {.db = db, .session_id = 1, .agent_name = "test"};
    tool_configure_provider_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "configure_provider");
    assert(e);
    char *result = e->handler(
        "{\"provider\":\"openrouter\",\"api_key\":\"sk-test-123\"}",
        e->user_data);
    assert(result);
    assert(strncmp(result, SENTINEL_CONFIG, strlen(SENTINEL_CONFIG)) == 0);
    free(result);
    tools_free(&reg);
    db_close(db);
    printf("  PASS: configure_provider returns SENTINEL_CONFIG\n");
}

/* T197: configure_channel returns SENTINEL_CONFIG prefix */
static void test_configure_channel_sentinel(void) {
    sqlite3 *db = setup();
    ToolRegistry reg;
    tools_init(&reg);
    ToolBootstrapCtx ctx = {.db = db, .session_id = 1, .agent_name = "test"};
    tool_configure_channel_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "configure_channel");
    assert(e);
    char *result = e->handler("{\"channel_type\":\"cli\"}", e->user_data);
    assert(result);
    assert(strncmp(result, SENTINEL_CONFIG, strlen(SENTINEL_CONFIG)) == 0);
    free(result);
    tools_free(&reg);
    db_close(db);
    printf("  PASS: configure_channel returns SENTINEL_CONFIG\n");
}

/* T197: launch_agent blocking in daemon mode returns SENTINEL_SPAWN prefix */
static void test_spawn_sentinel(void) {
    sqlite3 *db = setup();
    ToolRegistry reg;
    tools_init(&reg);
    AgentLaunchCtx ctx = {.db = db, .session_id = 1, .daemon_mode = 1};
    tool_launch_agent_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "launch_agent");
    assert(e);
    char *result = e->handler("{\"task\":\"do something\"}", e->user_data);
    assert(result);
    assert(strncmp(result, SENTINEL_SPAWN, strlen(SENTINEL_SPAWN)) == 0);
    free(result);
    tools_free(&reg);
    db_close(db);
    printf("  PASS: launch_agent blocking returns SENTINEL_SPAWN\n");
}

/* T201: launch_agent background also returns SENTINEL_SPAWN (V13) */
static void test_spawn_background_sentinel(void) {
    sqlite3 *db = setup();
    ToolRegistry reg;
    tools_init(&reg);
    AgentLaunchCtx ctx = {.db = db, .session_id = 1, .daemon_mode = 1};
    tool_launch_agent_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "launch_agent");
    assert(e);
    char *result = e->handler("{\"task\":\"bg task\",\"background\":true}", e->user_data);
    assert(result);
    assert(strncmp(result, SENTINEL_SPAWN, strlen(SENTINEL_SPAWN)) == 0);
    assert(strstr(result, "background") != NULL);
    free(result);
    tools_free(&reg);
    db_close(db);
    printf("  PASS: launch_agent background returns SENTINEL_SPAWN (V13)\n");
}

int main(void) {
    printf("test_agent_exit:\n");
    test_exit_codes();
    test_approval_sentinel();
    test_configure_provider_sentinel();
    test_configure_channel_sentinel();
    test_spawn_sentinel();
    test_spawn_background_sentinel();
    printf("ALL PASSED\n");
    return 0;
}
