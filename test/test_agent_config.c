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
    test_db_clean("/tmp/test_agent_wl.db");
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
    test_db_clean("/tmp/test_agent_wl.db");
    printf("  PASS: test_whitelist_add_remove\n");
}

static void test_add_tool(void) {
    test_db_clean("/tmp/test_agent_tool.db");
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
    test_db_clean("/tmp/test_agent_tool.db");
    printf("  PASS: test_add_tool\n");
}

/* cclaw.db is never reachable from inside the sandbox, so a path grant that
 * would expose it is refused at grant time — loudly, with nothing written.
 * Silently masking an approved grant at mount time is the failure class this
 * replaces: an approved grant that quietly does nothing.
 *
 * Every refusal here is paired with a positive control in the same function,
 * so a bug that refuses *everything* cannot pass this test. */
static void test_db_path_grant_refused(void) {
    const char *dbp = "/tmp/cclaw_grantdb/cclaw.db";
    system("rm -rf /tmp/cclaw_grantdb /tmp/cclaw_grantok"
           " && mkdir -p /tmp/cclaw_grantdb /tmp/cclaw_grantok");
    sqlite3 *db = test_db_open(dbp);
    assert(db != NULL);
    db_agent_upsert(db, "bot", NULL, NULL);

    /* Positive control: ordinary paths are grantable, read and write. */
    assert(agent_config_grant(db, "bot", "read_path", "/usr/share", 0) == 0);
    assert(agent_config_grant(db, "bot", "write_path", "/tmp/cclaw_grantok", 0) == 0);

    /* The DB file itself, either flag — the ro/rw distinction is irrelevant,
     * read is reconnaissance on the policy store and write is escalation. */
    assert(agent_config_grant(db, "bot", "read_path", dbp, 0) != 0);
    assert(agent_config_grant(db, "bot", "write_path", dbp, 0) != 0);

    /* Any ancestor directory, not just the immediate one. */
    assert(agent_config_grant(db, "bot", "read_path", "/tmp", 0) != 0);

    /* The directory containing it, and the -wal/-shm siblings. */
    assert(agent_config_grant(db, "bot", "read_path", "/tmp/cclaw_grantdb", 0) != 0);
    assert(agent_config_grant(db, "bot", "read_path",
                              "/tmp/cclaw_grantdb/cclaw.db-wal", 0) != 0);
    assert(agent_config_grant(db, "bot", "read_path",
                              "/tmp/cclaw_grantdb/cclaw.db-shm", 0) != 0);

    /* A path that does not exist yet still resolves through its nearest
     * existing ancestor — otherwise "grant me ~/.cclaw/x" walks straight in. */
    assert(agent_config_grant(db, "bot", "read_path",
                              "/tmp/cclaw_grantdb/nested/deep", 0) != 0);

    /* Non-path kinds are untouched by the rule. */
    assert(agent_config_grant(db, "bot", "tool", "shell_exec", 0) == 0);

    /* Nothing was written for any refused grant: only the three accepted. */
    AgentCaps caps;
    agent_caps_load(db, "bot", &caps);
    assert(caps.read_count == 1);
    assert(caps.write_count == 1);
    agent_caps_free(&caps);

    db_close(db);
    system("rm -rf /tmp/cclaw_grantdb /tmp/cclaw_grantok");
    printf("  PASS: test_db_path_grant_refused\n");
}

int main(void) {
    TEST_INIT();
    printf("test_agent_config:\n");
    test_whitelist_add_remove();
    test_add_tool();
    test_db_path_grant_refused();
    printf("all agent_config tests passed\n");
    return 0;
}
