#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "tool_soul.h"
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

/* T120: soul_edit updates agent soul in DB */
static void test_soul_edit_basic(void) {
    sqlite3 *db = setup();

    /* Seed an agent */
    int rc = db_agent_upsert(db, "testbot", NULL, NULL, NULL, NULL, NULL);
    assert(rc == 0);

    /* Register tool */
    ToolRegistry reg;
    tools_init(&reg);
    ToolSoulCtx ctx = {.db = db, .agent_name = "testbot"};
    tool_soul_register(&reg, &ctx);

    /* Call soul_edit */
    ToolEntry *e = tools_lookup(&reg, "soul_edit");
    assert(e);
    char *result = e->handler("{\"soul\":\"I am a helpful coder.\"}", e->user_data);
    assert(result);
    assert(strstr(result, "ok") != NULL);
    free(result);

    /* Verify DB updated */
    AgentRow *row = db_agent_get(db, "testbot");
    assert(row);
    assert(row->soul);
    assert(strcmp(row->soul, "I am a helpful coder.") == 0);
    agent_row_free(row);

    tools_free(&reg);
    db_close(db);
    printf("PASS: test_soul_edit_basic\n");
}

/* T120: soul_edit fails for nonexistent agent */
static void test_soul_edit_no_agent(void) {
    sqlite3 *db = setup();

    ToolRegistry reg;
    tools_init(&reg);
    ToolSoulCtx ctx = {.db = db, .agent_name = "ghost"};
    tool_soul_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "soul_edit");
    assert(e);
    char *result = e->handler("{\"soul\":\"test\"}", e->user_data);
    assert(result);
    assert(strstr(result, "error") != NULL);
    free(result);

    tools_free(&reg);
    db_close(db);
    printf("PASS: test_soul_edit_no_agent\n");
}

/* T120: soul_edit with missing field */
static void test_soul_edit_missing_field(void) {
    sqlite3 *db = setup();

    ToolRegistry reg;
    tools_init(&reg);
    ToolSoulCtx ctx = {.db = db, .agent_name = "testbot"};
    tool_soul_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "soul_edit");
    char *result = e->handler("{}", e->user_data);
    assert(result);
    assert(strstr(result, "error") != NULL);
    free(result);

    tools_free(&reg);
    db_close(db);
    printf("PASS: test_soul_edit_missing_field\n");
}

int main(void) {
    test_soul_edit_basic();
    test_soul_edit_no_agent();
    test_soul_edit_missing_field();
    printf("All soul_edit tests passed.\n");
    return 0;
}
