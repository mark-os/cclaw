/* Dynamic system prompt: generated Your Tools / Requestable Tools sections
 * from grants ∩ tools (deterministic, ORDER BY name), and the {workspace}
 * template var (review-1 F21). */
#include "db.h"
#include "test_util.h"
#include "agent_config.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define TEST_DB "/tmp/test_cclaw_dynprompt.sqlite"

static char *build(sqlite3 *db, const char *agent) {
    Config cfg = {0};
    cfg.context_window = 128000;
    char *p = agent_build_system_prompt(db, agent, 1, "/srv/agents", &cfg);
    assert(p);
    return p;
}

int main(void) {
    TEST_INIT();
    printf("test_dynamic_prompt:\n");
    test_db_clean(TEST_DB);
    sqlite3 *db = test_db_open(TEST_DB);
    assert(db);

    db_agent_upsert(db, "Tester", NULL,
                    "You are {agent_name}. Workspace: {workspace}");
    assert(sqlite3_exec(db,
        "INSERT INTO tools(name, description, parameters_json, enabled) VALUES"
        " ('alpha_tool','does alpha','{}',1),"
        " ('beta_tool','does beta','{}',1),"
        " ('gamma_tool','disabled tool','{}',0);"
        "INSERT INTO grants(agent_name, kind, value) VALUES"
        " ('Tester','tool','alpha_tool');",
        NULL, NULL, NULL) == SQLITE_OK);

    /* granted → Your Tools; ungranted → Requestable; disabled → absent */
    printf("  sections... ");
    char *p = build(db, "Tester");
    char *yours = strstr(p, "## Your Tools");
    char *req = strstr(p, "## Requestable Tools");
    assert(yours && req && yours < req);
    char *alpha = strstr(p, "- alpha_tool — does alpha");
    char *beta = strstr(p, "- beta_tool — does beta");
    assert(alpha && alpha > yours && alpha < req);
    assert(beta && beta > req);
    assert(!strstr(p, "gamma_tool"));
    assert(strstr(p, "request_config"));
    printf("PASS\n");

    /* {workspace} renders as <agents_dir>/<agent>/workspace (F21) */
    printf("  workspace_var... ");
    assert(strstr(p, "Workspace: /srv/agents/Tester/workspace"));
    assert(strstr(p, "You are Tester."));
    free(p);
    printf("PASS\n");

    /* revoke → moves to requestable on the next build */
    printf("  revoke_moves... ");
    assert(sqlite3_exec(db,
        "DELETE FROM grants WHERE agent_name='Tester' AND value='alpha_tool';",
        NULL, NULL, NULL) == SQLITE_OK);
    p = build(db, "Tester");
    assert(!strstr(p, "## Your Tools"));   /* nothing granted now */
    req = strstr(p, "## Requestable Tools");
    assert(req && strstr(p, "- alpha_tool — does alpha") > req);
    free(p);
    printf("PASS\n");

    /* expired grant counts as ungranted */
    printf("  expired_grant... ");
    assert(sqlite3_exec(db,
        "INSERT INTO grants(agent_name, kind, value, expires_at)"
        " VALUES('Tester','tool','beta_tool', unixepoch()-10);",
        NULL, NULL, NULL) == SQLITE_OK);
    p = build(db, "Tester");
    assert(!strstr(p, "## Your Tools"));
    free(p);
    printf("PASS\n");

    /* determinism: two builds byte-identical */
    printf("  deterministic... ");
    p = build(db, "Tester");
    char *q = build(db, "Tester");
    assert(strcmp(p, q) == 0);
    free(p); free(q);
    printf("PASS\n");

    db_close(db);
    test_db_clean(TEST_DB);
    printf("all dynamic_prompt tests passed\n");
    return 0;
}
