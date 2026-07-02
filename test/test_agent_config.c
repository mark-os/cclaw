/* Test agent config grant/revoke/caps API */
#define _POSIX_C_SOURCE 200809L
#include "agent_config.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_whitelist_add_remove(void) {
    unlink("/tmp/test_agent_wl.db");
    sqlite3 *db = test_db_open("/tmp/test_agent_wl.db");
    assert(db != NULL);
    db_agent_upsert(db, "bot", NULL, NULL);

    assert(agent_config_grant(db, "bot", "host", "api.example.com", 0) == 0);
    AgentCaps caps;
    agent_caps_load(db, "bot", &caps);
    assert(caps.host_count == 1);
    assert(strcmp(caps.hosts[0], "api.example.com") == 0);
    agent_caps_free(&caps);

    /* Duplicate is no-op */
    assert(agent_config_grant(db, "bot", "host", "api.example.com", 0) == 0);
    agent_caps_load(db, "bot", &caps);
    assert(caps.host_count == 1);
    agent_caps_free(&caps);

    /* Add second */
    assert(agent_config_grant(db, "bot", "host", "api.github.com", 0) == 0);
    agent_caps_load(db, "bot", &caps);
    assert(caps.host_count == 2);
    agent_caps_free(&caps);

    /* Remove */
    assert(agent_config_revoke(db, "bot", "host", "api.example.com") == 0);
    agent_caps_load(db, "bot", &caps);
    assert(caps.host_count == 1);
    assert(strcmp(caps.hosts[0], "api.github.com") == 0);
    agent_caps_free(&caps);

    db_close(db);
    unlink("/tmp/test_agent_wl.db");
    printf("  PASS: test_whitelist_add_remove\n");
}

static void test_add_tool(void) {
    unlink("/tmp/test_agent_tool.db");
    sqlite3 *db = test_db_open("/tmp/test_agent_tool.db");
    assert(db);
    db_agent_upsert(db, "bot", NULL, NULL);

    assert(agent_config_grant(db, "bot", "tool", "shell_exec", 0) == 0);
    assert(agent_config_grant(db, "bot", "tool", "web_fetch", 0) == 0);

    AgentCaps caps;
    agent_caps_load(db, "bot", &caps);
    assert(caps.tool_count == 2);
    /* Both present (order-independent) */
    int found_shell = 0, found_web = 0;
    for (size_t i = 0; i < caps.tool_count; i++) {
        if (strcmp(caps.tools[i], "shell_exec") == 0) found_shell = 1;
        if (strcmp(caps.tools[i], "web_fetch") == 0) found_web = 1;
    }
    assert(found_shell && found_web);
    agent_caps_free(&caps);

    db_close(db);
    unlink("/tmp/test_agent_tool.db");
    printf("  PASS: test_add_tool\n");
}

int main(void) {
    printf("test_agent_config:\n");
    test_whitelist_add_remove();
    test_add_tool();
    printf("all agent_config tests passed\n");
    return 0;
}
