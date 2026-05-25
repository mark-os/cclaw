#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "tool_memory.h"
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

/* T121: memory_set updates agent memory in DB */
static void test_memory_set_basic(void) {
    sqlite3 *db = setup();
    int rc = db_agent_upsert(db, "testbot", NULL, NULL, NULL, NULL, NULL);
    assert(rc == 0);

    ToolRegistry reg;
    tools_init(&reg);
    ToolMemoryCtx ctx = {.db = db, .agent_name = "testbot"};
    tool_memory_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "memory_set");
    assert(e);
    char *result = e->handler("{\"memory\":\"User prefers concise answers.\"}", e->user_data);
    assert(result);
    assert(strstr(result, "ok") != NULL);
    free(result);

    /* Verify DB updated */
    AgentRow *row = db_agent_get(db, "testbot");
    assert(row);
    assert(row->memory);
    assert(strcmp(row->memory, "User prefers concise answers.") == 0);
    agent_row_free(row);

    tools_free(&reg);
    db_close(db);
    printf("PASS: test_memory_set_basic\n");
}

/* T121: memory_set fails for nonexistent agent */
static void test_memory_set_no_agent(void) {
    sqlite3 *db = setup();

    ToolRegistry reg;
    tools_init(&reg);
    ToolMemoryCtx ctx = {.db = db, .agent_name = "ghost"};
    tool_memory_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "memory_set");
    assert(e);
    char *result = e->handler("{\"memory\":\"test\"}", e->user_data);
    assert(result);
    assert(strstr(result, "error") != NULL);
    free(result);

    tools_free(&reg);
    db_close(db);
    printf("PASS: test_memory_set_no_agent\n");
}

/* T121: memory_set with missing field */
static void test_memory_set_missing_field(void) {
    sqlite3 *db = setup();

    ToolRegistry reg;
    tools_init(&reg);
    ToolMemoryCtx ctx = {.db = db, .agent_name = "testbot"};
    tool_memory_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "memory_set");
    char *result = e->handler("{}", e->user_data);
    assert(result);
    assert(strstr(result, "error") != NULL);
    free(result);

    tools_free(&reg);
    db_close(db);
    printf("PASS: test_memory_set_missing_field\n");
}

int main(void) {
    test_memory_set_basic();
    test_memory_set_no_agent();
    test_memory_set_missing_field();
    printf("All memory_set tests passed.\n");
    return 0;
}
