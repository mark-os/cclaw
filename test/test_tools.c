#include "tools.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *dummy_handler(const char *arguments, void *user_data, int *is_error) {
    (void)arguments;
    (void)user_data;
    (void)is_error;
    char *r = malloc(8);
    if (r) strcpy(r, "ok");
    return r;
}

static void test_init(void) {
    ToolRegistry reg;
    tools_init(&reg);
    assert(reg.count == 0);
    printf("  PASS test_init\n");
}

static void test_register_and_lookup(void) {
    ToolRegistry reg;
    tools_init(&reg);

    int rc = tools_register(&reg, "shell_exec", "Run a command",
                            "{\"type\":\"object\"}", dummy_handler, NULL);
    assert(rc == 0);
    assert(reg.count == 1);

    ToolEntry *e = tools_lookup(&reg, "shell_exec");
    assert(e != NULL);
    assert(strcmp(e->name, "shell_exec") == 0);
    assert(strcmp(e->description, "Run a command") == 0);
    assert(e->handler == dummy_handler);

    tools_free(&reg);
    printf("  PASS test_register_and_lookup\n");
}

static void test_lookup_miss(void) {
    ToolRegistry reg;
    tools_init(&reg);

    tools_register(&reg, "file_read", NULL, NULL, dummy_handler, NULL);
    ToolEntry *e = tools_lookup(&reg, "nonexistent");
    assert(e == NULL);

    tools_free(&reg);
    printf("  PASS test_lookup_miss\n");
}

static void test_register_full(void) {
    ToolRegistry reg;
    tools_init(&reg);

    for (int i = 0; i < TOOLS_MAX; i++) {
        char name[16];
        snprintf(name, sizeof(name), "tool_%d", i);
        int rc = tools_register(&reg, name, NULL, NULL, dummy_handler, NULL);
        assert(rc == 0);
    }
    /* Registry full — next register should fail */
    int rc = tools_register(&reg, "overflow", NULL, NULL, dummy_handler, NULL);
    assert(rc == -1);
    assert(reg.count == TOOLS_MAX);

    tools_free(&reg);
    printf("  PASS test_register_full\n");
}

static void test_handler_dispatch(void) {
    ToolRegistry reg;
    tools_init(&reg);

    tools_register(&reg, "test_tool", NULL, NULL, dummy_handler, NULL);
    ToolEntry *e = tools_lookup(&reg, "test_tool");
    assert(e != NULL);

    char *result = e->handler("{}", e->user_data, &(int){0});
    assert(result != NULL);
    assert(strcmp(result, "ok") == 0);
    free(result);

    tools_free(&reg);
    printf("  PASS test_handler_dispatch\n");
}

static int free_fn_called = 0;
static void test_free_fn(void *data) {
    (void)data;
    free_fn_called = 1;
}


static void test_free_fn_called_on_cleanup(void) {
    ToolRegistry reg;
    tools_init(&reg);
    tools_register(&reg, "test", NULL, NULL, dummy_handler, (void *)0x1);
    reg.entries[0].free_fn = test_free_fn;

    free_fn_called = 0;
    tools_free(&reg);
    assert(free_fn_called == 1);
    printf("  PASS test_free_fn_called_on_cleanup\n");
}

static void test_register_null_name(void) {
    ToolRegistry reg;
    tools_init(&reg);
    int rc = tools_register(&reg, NULL, "desc", NULL, dummy_handler, NULL);
    assert(rc == -1);
    assert(reg.count == 0);
    printf("  PASS test_register_null_name\n");
}

static void test_register_null_reg(void) {
    int rc = tools_register(NULL, "test", NULL, NULL, dummy_handler, NULL);
    assert(rc == -1);
    printf("  PASS test_register_null_reg\n");
}

static void test_lookup_null_args(void) {
    ToolRegistry reg;
    tools_init(&reg);
    assert(tools_lookup(NULL, "x") == NULL);
    assert(tools_lookup(&reg, NULL) == NULL);
    printf("  PASS test_lookup_null_args\n");
}


static const char *TOOLS_DB_PATH = "/tmp/test_cclaw_tools_ext.db";

/* tools_load_extension_tools: loads extension tools into the registry from
 * the DB, and calling it twice is idempotent (no duplicate entries). Also
 * verifies that a second agent with no attachments registers nothing. */
static void test_load_extension_tools_idempotent(void) {
    test_db_clean(TOOLS_DB_PATH);
    sqlite3 *db = test_db_open(TOOLS_DB_PATH);
    assert(db);
    /* agent_extensions.agent_name FK parent. */
    test_seed_agent(db, "Alice");

    /* Seed the required DB rows: extensions, agent_extensions, tools. */
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
    assert(reg.count == 0);

    /* First load: both extension tools should appear. */
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
    test_db_clean(TOOLS_DB_PATH);
    printf("  PASS test_load_extension_tools_idempotent\n");
}

int main(void) {
    TEST_INIT();
    printf("test_tools:\n");
    test_init();
    test_register_and_lookup();
    test_lookup_miss();
    test_register_full();
    test_handler_dispatch();
    test_free_fn_called_on_cleanup();
    test_register_null_name();
    test_register_null_reg();
    test_lookup_null_args();
    test_load_extension_tools_idempotent();
    printf("All tool registry tests passed.\n");
    return 0;
}
