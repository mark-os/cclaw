#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "tool_approval.h"
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

/* T147: approval_request inserts into approvals table */
static void test_approval_basic(void) {
    sqlite3 *db = setup();

    ToolRegistry reg;
    tools_init(&reg);
    ToolApprovalCtx ctx = {.db = db, .session_id = 1, .agent_name = "coder"};
    tool_approval_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "approval_request");
    assert(e);
    char *result = e->handler(
        "{\"type\":\"whitelist_host\",\"payload\":{\"host\":\"api.example.com\"}}",
        e->user_data);
    assert(result);
    assert(strstr(result, "pending approval") != NULL);
    assert(strstr(result, "id=") != NULL);
    free(result);

    /* Verify row in DB */
    int count = 0;
    Approval *list = approval_list_pending(db, &count);
    assert(count == 1);
    assert(strcmp(list[0].agent_name, "coder") == 0);
    assert(strcmp(list[0].type, "whitelist_host") == 0);
    assert(strstr(list[0].payload, "api.example.com") != NULL);
    approval_list_free(list, count);

    tools_free(&reg);
    db_close(db);
    printf("PASS: test_approval_basic\n");
}

/* T147: invalid type rejected */
static void test_approval_invalid_type(void) {
    sqlite3 *db = setup();

    ToolRegistry reg;
    tools_init(&reg);
    ToolApprovalCtx ctx = {.db = db, .session_id = 1, .agent_name = "coder"};
    tool_approval_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "approval_request");
    char *result = e->handler(
        "{\"type\":\"invalid_type\",\"payload\":{}}",
        e->user_data);
    assert(result);
    assert(strstr(result, "error") != NULL);
    free(result);

    tools_free(&reg);
    db_close(db);
    printf("PASS: test_approval_invalid_type\n");
}

/* T147: missing type field */
static void test_approval_missing_type(void) {
    sqlite3 *db = setup();

    ToolRegistry reg;
    tools_init(&reg);
    ToolApprovalCtx ctx = {.db = db, .session_id = 1, .agent_name = "coder"};
    tool_approval_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "approval_request");
    char *result = e->handler("{\"payload\":{}}", e->user_data);
    assert(result);
    assert(strstr(result, "error") != NULL);
    free(result);

    tools_free(&reg);
    db_close(db);
    printf("PASS: test_approval_missing_type\n");
}

int main(void) {
    test_approval_basic();
    test_approval_invalid_type();
    test_approval_missing_type();
    printf("All approval_request tests passed.\n");
    return 0;
}
