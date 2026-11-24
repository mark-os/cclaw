#include "tools.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>

static char *dummy_handler(const char *arguments, void *user_data, int *is_error) {
    (void)arguments;
    (void)user_data;
    (void)is_error;
    char *r = malloc(8);
    if (r) strcpy(r, "ok");
    return r;
}

static int free_fn_called = 0;
static void test_free_fn(void *data) {
    (void)data;
    free_fn_called = 1;
}

/* Smoke: register, look up, and free (which runs the entry's free_fn). */
static void test_registry(void) {
    ToolRegistry reg;
    tools_init(&reg);
    assert(tools_register(&reg, "shell_exec", "Run a command",
                          "{\"type\":\"object\"}", dummy_handler, (void *)0x1) == 0);
    ToolEntry *e = tools_lookup(&reg, "shell_exec");
    assert(e && e->handler == dummy_handler);
    assert(tools_lookup(&reg, "nonexistent") == NULL);
    e->free_fn = test_free_fn;
    tools_free(&reg);
    assert(free_fn_called == 1);
}

/* tools_load_extension_tools: loads extension tools into the registry from
 * the DB, and calling it twice is idempotent (no duplicate entries). Also
 * verifies that a second agent with no attachments registers nothing. */
static void test_load_extension_tools_idempotent(void) {
    /* A file DB: the shared extension store is resolved from the DB path. */
    static const char *path = "/tmp/test_cclaw_tools_ext.db";
    test_db_clean(path);
    sqlite3 *db = test_db_open(path);
    assert(db);
    /* agent_extensions.agent_name FK parent. */
    test_seed_agent(db, "Alice");

    int rc = sqlite3_exec(db,
        "INSERT INTO extensions(name, path, owner_agent, published, enabled)"
        " VALUES('weather','/tmp/extensions/weather','Alice',1,1);"
        "INSERT INTO agent_extensions(agent_name, extension_name, enabled)"
        " VALUES('Alice','weather',1);"
        "INSERT INTO tools(name, extension_name, description, parameters_json, path, enabled)"
        " VALUES('get_forecast','weather','Get forecast',"
        "'{\"type\":\"object\",\"properties\":{\"lat\":{\"type\":\"number\"}}}','/tmp/extensions/weather/forecast.qjs',1);"
        "INSERT INTO tools(name, extension_name, description, parameters_json, path, enabled)"
        " VALUES('get_alerts','weather','Get alerts',"
        "'{\"type\":\"object\"}','/tmp/extensions/weather/alerts.qjs',1);",
        NULL, NULL, NULL);
    assert(rc == SQLITE_OK);

    ToolRegistry reg;
    tools_init(&reg);

    tools_load_extension_tools(&reg, db, "Alice", NULL);
    assert(reg.count == 2);
    assert(tools_lookup(&reg, "get_forecast") != NULL);
    assert(tools_lookup(&reg, "get_alerts") != NULL);

    /* Second load (idempotent): count must not grow. */
    tools_load_extension_tools(&reg, db, "Alice", NULL);
    assert(reg.count == 2);

    /* A different agent with no attachments registers nothing. */
    tools_load_extension_tools(&reg, db, "Bob", NULL);
    assert(reg.count == 2);

    /* Copy-on-promote (Q4): a row whose handler path is an agent workspace
     * draft — not the shared store — is skipped, so approved code can never
     * be swapped for the agent's editable copy after the approval. The two
     * rows above are the positive control: same query, same attachment, they
     * load because their paths are in the store. */
    rc = sqlite3_exec(db,
        "INSERT INTO tools(name, extension_name, description, parameters_json, path, enabled)"
        " VALUES('get_draft','weather','Draft','{\"type\":\"object\"}',"
        "'/tmp/agents/Alice/workspace/extensions/weather/draft.qjs',1);",
        NULL, NULL, NULL);
    assert(rc == SQLITE_OK);
    tools_load_extension_tools(&reg, db, "Alice", NULL);
    assert(tools_lookup(&reg, "get_draft") == NULL);
    assert(reg.count == 2);

    tools_free(&reg);
    db_close(db);
    test_db_clean(path);
}

int main(void) {
    TEST_INIT();
    RUN(test_registry);
    RUN(test_load_extension_tools_idempotent);
    return 0;
}
