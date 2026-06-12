#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "test_util.h"
#include "tool_memory.h"
#include "tools.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* T153: memory tools supersede old memory_set (T121) */

static sqlite3 *setup(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    return db;
}

static void test_create_and_append(void) {
    sqlite3 *db = setup();
    ToolRegistry reg;
    tools_init(&reg);
    ToolMemoryCtx ctx = {.db = db, .agent_name = "bot"};
    tool_memory_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "memory_create");
    assert(e);
    char *r = e->handler("{\"label\":\"facts\",\"description\":\"user facts\"}", e->user_data);
    assert(strstr(r, "ok"));
    free(r);

    e = tools_lookup(&reg, "memory_append");
    assert(e);
    r = e->handler("{\"label\":\"facts\",\"content\":\"likes coffee\"}", e->user_data);
    assert(strstr(r, "ok"));
    free(r);

    MemoryBlock *mb = memory_block_get(db, "bot", "facts");
    assert(mb && strcmp(mb->value, "likes coffee") == 0);
    memory_block_free(mb);

    tools_free(&reg);
    db_close(db);
    printf("PASS: test_create_and_append\n");
}

static void test_replace(void) {
    sqlite3 *db = setup();
    ToolRegistry reg;
    tools_init(&reg);
    ToolMemoryCtx ctx = {.db = db, .agent_name = "bot"};
    tool_memory_register(&reg, &ctx);

    ToolEntry *create = tools_lookup(&reg, "memory_create");
    char *r = create->handler("{\"label\":\"bio\",\"description\":\"about\",\"value\":\"age 30\"}", create->user_data);
    assert(strstr(r, "ok"));
    free(r);

    ToolEntry *replace = tools_lookup(&reg, "memory_replace");
    r = replace->handler("{\"label\":\"bio\",\"old\":\"30\",\"new\":\"31\"}", replace->user_data);
    assert(strstr(r, "ok"));
    free(r);

    MemoryBlock *mb = memory_block_get(db, "bot", "bio");
    assert(mb && strcmp(mb->value, "age 31") == 0);
    memory_block_free(mb);

    tools_free(&reg);
    db_close(db);
    printf("PASS: test_replace\n");
}

static void test_missing_fields(void) {
    sqlite3 *db = setup();
    ToolRegistry reg;
    tools_init(&reg);
    ToolMemoryCtx ctx = {.db = db, .agent_name = "bot"};
    tool_memory_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "memory_create");
    char *r = e->handler("{}", e->user_data);
    assert(strstr(r, "error"));
    free(r);

    e = tools_lookup(&reg, "memory_append");
    r = e->handler("{\"label\":\"x\"}", e->user_data);
    assert(strstr(r, "error"));
    free(r);

    tools_free(&reg);
    db_close(db);
    printf("PASS: test_missing_fields\n");
}

int main(void) {
    test_create_and_append();
    test_replace();
    test_missing_fields();
    printf("All memory tool tests passed.\n");
    return 0;
}
