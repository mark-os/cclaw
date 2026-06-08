/* T274: Unit test for request_config tool + agent_config_add_tool */
#include "db.h"
#include "tools.h"
#include "tool_request_config.h"
#include "agent_config.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static void test_register(void) {
    ToolRegistry reg;
    tools_init(&reg);
    int rc = tool_request_config_register(&reg);
    assert(rc == 0);

    ToolEntry *e = tools_lookup(&reg, "request_config");
    assert(e != NULL);
    assert(strcmp(e->name, "request_config") == 0);
    assert(e->handler != NULL);

    tools_free(&reg);
    printf("  PASS test_register\n");
}

/* In the new architecture, request_config handler is never called directly
 * (turn_loop handles it inline). But if called, it returns an error. */
static void test_handler_returns_error(void) {
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg);

    ToolEntry *e = tools_lookup(&reg, "request_config");
    char *result = e->handler("{\"action\":\"grant_tool\",\"tool\":\"shell_exec\"}", e->user_data);
    assert(result != NULL);
    assert(strstr(result, "error") != NULL || strstr(result, "parent") != NULL);
    free(result);

    tools_free(&reg);
    printf("  PASS test_handler_returns_error\n");
}

static void test_missing_tool_field(void) {
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg);

    ToolEntry *e = tools_lookup(&reg, "request_config");
    char *result = e->handler("{\"action\":\"grant_tool\"}", e->user_data);
    assert(result != NULL);
    assert(strstr(result, "error") != NULL);
    free(result);

    tools_free(&reg);
    printf("  PASS test_missing_tool_field\n");
}

static void test_add_tool_to_config(void) {
    sqlite3 *db = db_open(":memory:");
    assert(db);
    db_agent_upsert(db, "test", NULL, NULL, NULL);

    int rc = agent_config_add_tool(db, "test", "shell_exec");
    assert(rc == 0);

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT allowed_tools FROM agents WHERE name='test'",
                       -1, &stmt, NULL);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    const char *val = (const char *)sqlite3_column_text(stmt, 0);
    cJSON *arr = cJSON_Parse(val);
    assert(arr && cJSON_GetArraySize(arr) == 1);
    cJSON_Delete(arr);
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
