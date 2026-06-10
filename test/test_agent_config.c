/* Test agent config functions on new schema */
#define _POSIX_C_SOURCE 200809L
#include "agent_config.h"
#include "db.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_whitelist_add_remove(void) {
    unlink("/tmp/test_agent_wl.db");
    sqlite3 *db = test_db_open("/tmp/test_agent_wl.db");
    assert(db != NULL);
    db_agent_upsert(db, "bot", NULL, NULL, NULL);

    assert(agent_config_add_host(db, "bot", "api.example.com") == 0);
    size_t count = 0;
    char **hosts = agent_config_get_hosts(db, "bot", &count);
    assert(count == 1);
    assert(strcmp(hosts[0], "api.example.com") == 0);
    free(hosts[0]); free(hosts);

    /* Duplicate is no-op */
    assert(agent_config_add_host(db, "bot", "api.example.com") == 0);
    hosts = agent_config_get_hosts(db, "bot", &count);
    assert(count == 1);
    free(hosts[0]); free(hosts);

    /* Add second */
    assert(agent_config_add_host(db, "bot", "api.github.com") == 0);
    hosts = agent_config_get_hosts(db, "bot", &count);
    assert(count == 2);
    free(hosts[0]); free(hosts[1]); free(hosts);

    /* Remove */
    assert(agent_config_remove_host(db, "bot", "api.example.com") == 0);
    hosts = agent_config_get_hosts(db, "bot", &count);
    assert(count == 1);
    assert(strcmp(hosts[0], "api.github.com") == 0);
    free(hosts[0]); free(hosts);

    db_close(db);
    unlink("/tmp/test_agent_wl.db");
    printf("  PASS: test_whitelist_add_remove\n");
}

static void test_add_tool(void) {
    unlink("/tmp/test_agent_tool.db");
    sqlite3 *db = test_db_open("/tmp/test_agent_tool.db");
    assert(db);
    db_agent_upsert(db, "bot", NULL, NULL, NULL);

    assert(agent_config_add_tool(db, "bot", "shell_exec") == 0);
    assert(agent_config_add_tool(db, "bot", "web_fetch") == 0);

    /* Verify via load */
    AgentConfig *ac = agent_config_load_db(db, "bot");
    assert(ac);
    assert(ac->tool_count == 2);
    agent_config_free(ac);

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
