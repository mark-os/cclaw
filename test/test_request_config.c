/* T274: Unit test for request_config tool + agent_config_add_tool */
#include "db.h"
#include "test_util.h"
#include "tools.h"
#include "tool_request_config.h"
#include "agent_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static void test_register(void) {
    ToolRegistry reg;
    tools_init(&reg);
    int rc = tool_request_config_register(&reg, NULL);
    assert(rc == 0);

    ToolEntry *e = tools_lookup(&reg, "request_config");
    assert(e != NULL);
    assert(strcmp(e->name, "request_config") == 0);
    assert(e->handler != NULL);

    tools_free(&reg);
    printf("  PASS test_register\n");
}

/* With NULL context, handler returns unavailable error. */
static void test_handler_returns_error(void) {
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg, NULL);

    ToolEntry *e = tools_lookup(&reg, "request_config");
    char *result = e->handler("{\"action\":\"grant_tool\",\"tool\":\"shell_exec\"}", e->user_data);
    assert(result != NULL);
    assert(strstr(result, "error") != NULL);
    free(result);

    tools_free(&reg);
    printf("  PASS test_handler_returns_error\n");
}

static void test_missing_tool_field(void) {
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg, NULL);

    ToolEntry *e = tools_lookup(&reg, "request_config");
    char *result = e->handler("{\"action\":\"grant_tool\"}", e->user_data);
    assert(result != NULL);
    assert(strstr(result, "error") != NULL);
    free(result);

    tools_free(&reg);
    printf("  PASS test_missing_tool_field\n");
}

static void test_add_tool_to_config(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    db_agent_upsert(db, "test", NULL, NULL, NULL);

    int rc = agent_config_add_tool(db, "test", "shell_exec");
    assert(rc == 0);

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db,
        "SELECT json_array_length(allowed_tools) FROM agents WHERE name='test'",
        -1, &stmt, NULL);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 1);
    sqlite3_finalize(stmt);

    db_close(db);
    printf("  PASS test_add_tool_to_config\n");
}

int main(void) {
    printf("test_request_config (T274):\n");
    test_register();
    test_handler_returns_error();
    test_missing_tool_field();
    test_add_tool_to_config();
    printf("\nAll request_config tests passed.\n");
    return 0;
}
