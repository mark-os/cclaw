#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "db.h"
#include "tool_memory.h"

#define FAIL(msg) do { fprintf(stderr, "FAIL: %s\n", msg); exit(1); } while(0)

static sqlite3 *setup_db(void) {
    sqlite3 *db = db_open(":memory:");
    if (!db) { FAIL("db_open"); }
    return db;
}

static void test_memory_create(void) {
    sqlite3 *db = setup_db();
    ToolMemoryCtx ctx = {.db = db, .agent_name = "test_agent"};
    ToolRegistry reg;
    tools_init(&reg);
    tool_memory_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "memory_create");
    assert(e);

    /* Create with value */
    char *r = e->handler("{\"label\":\"notes\",\"description\":\"scratch pad\",\"value\":\"hello\"}", e->user_data);
    assert(strstr(r, "ok"));
    free(r);

    /* Verify in DB */
    MemoryBlock *mb = memory_block_get(db, "test_agent", "notes");
    assert(mb);
    assert(strcmp(mb->value, "hello") == 0);
    assert(strcmp(mb->description, "scratch pad") == 0);
    memory_block_free(mb);

    /* Duplicate label fails */
    r = e->handler("{\"label\":\"notes\",\"description\":\"dup\"}", e->user_data);
    assert(strstr(r, "error"));
    free(r);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_memory_create\n");
}

static void test_memory_append(void) {
    sqlite3 *db = setup_db();
    ToolMemoryCtx ctx = {.db = db, .agent_name = "test_agent"};
    ToolRegistry reg;
    tools_init(&reg);
    tool_memory_register(&reg, &ctx);

    /* Create block first */
    ToolEntry *create = tools_lookup(&reg, "memory_create");
    char *r = create->handler("{\"label\":\"log\",\"description\":\"event log\",\"value\":\"A\"}", create->user_data);
    assert(strstr(r, "ok"));
    free(r);

    /* Append */
    ToolEntry *append = tools_lookup(&reg, "memory_append");
    r = append->handler("{\"label\":\"log\",\"content\":\"B\"}", append->user_data);
    assert(strstr(r, "ok"));
    free(r);

    MemoryBlock *mb = memory_block_get(db, "test_agent", "log");
    assert(mb);
    assert(strcmp(mb->value, "AB") == 0);
    memory_block_free(mb);

    /* Append to nonexistent block */
    r = append->handler("{\"label\":\"nope\",\"content\":\"x\"}", append->user_data);
    assert(strstr(r, "error"));
    free(r);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_memory_append\n");
}

static void test_memory_replace(void) {
    sqlite3 *db = setup_db();
    ToolMemoryCtx ctx = {.db = db, .agent_name = "test_agent"};
    ToolRegistry reg;
    tools_init(&reg);
    tool_memory_register(&reg, &ctx);

    ToolEntry *create = tools_lookup(&reg, "memory_create");
    char *r = create->handler("{\"label\":\"info\",\"description\":\"facts\",\"value\":\"the cat sat\"}", create->user_data);
    assert(strstr(r, "ok"));
    free(r);

    ToolEntry *replace = tools_lookup(&reg, "memory_replace");
    r = replace->handler("{\"label\":\"info\",\"old\":\"cat\",\"new\":\"dog\"}", replace->user_data);
    assert(strstr(r, "ok"));
    free(r);

    MemoryBlock *mb = memory_block_get(db, "test_agent", "info");
    assert(mb);
    assert(strcmp(mb->value, "the dog sat") == 0);
    memory_block_free(mb);

    /* Old text not found */
    r = replace->handler("{\"label\":\"info\",\"old\":\"cat\",\"new\":\"bird\"}", replace->user_data);
    assert(strstr(r, "not found"));
    free(r);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_memory_replace\n");
}

static void test_memory_read_only(void) {
    sqlite3 *db = setup_db();
    ToolMemoryCtx ctx = {.db = db, .agent_name = "test_agent"};
    ToolRegistry reg;
    tools_init(&reg);
    tool_memory_register(&reg, &ctx);

    /* Create block then set read_only directly in DB */
    ToolEntry *create = tools_lookup(&reg, "memory_create");
    char *r = create->handler("{\"label\":\"locked\",\"description\":\"immutable\",\"value\":\"fixed\"}", create->user_data);
    assert(strstr(r, "ok"));
    free(r);

    const char *sql = "UPDATE memory_blocks SET read_only=1 WHERE label='locked';";
    sqlite3_exec(db, sql, NULL, NULL, NULL);

    /* Append should fail */
    ToolEntry *append = tools_lookup(&reg, "memory_append");
    r = append->handler("{\"label\":\"locked\",\"content\":\" more\"}", append->user_data);
    assert(strstr(r, "read-only"));
    free(r);

    /* Replace should fail */
    ToolEntry *replace = tools_lookup(&reg, "memory_replace");
    r = replace->handler("{\"label\":\"locked\",\"old\":\"fixed\",\"new\":\"changed\"}", replace->user_data);
    assert(strstr(r, "read-only"));
    free(r);

    /* Value unchanged */
    MemoryBlock *mb = memory_block_get(db, "test_agent", "locked");
    assert(mb);
    assert(strcmp(mb->value, "fixed") == 0);
    memory_block_free(mb);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_memory_read_only\n");
}

static void test_memory_char_limit(void) {
    sqlite3 *db = setup_db();
    ToolMemoryCtx ctx = {.db = db, .agent_name = "test_agent"};
    ToolRegistry reg;
    tools_init(&reg);
    tool_memory_register(&reg, &ctx);

    /* Create block with small char_limit via DB directly */
    memory_block_create(db, "test_agent", "tiny", "small block", "hi", 5);

    /* Append that would exceed limit */
    ToolEntry *append = tools_lookup(&reg, "memory_append");
    char *r = append->handler("{\"label\":\"tiny\",\"content\":\"1234\"}", append->user_data);
    assert(strstr(r, "exceed"));
    free(r);

    /* Replace that would exceed limit */
    ToolEntry *replace = tools_lookup(&reg, "memory_replace");
    r = replace->handler("{\"label\":\"tiny\",\"old\":\"hi\",\"new\":\"hello world\"}", replace->user_data);
    assert(strstr(r, "exceed"));
    free(r);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_memory_char_limit\n");
}

int main(void) {
    printf("test_tool_memory_blocks:\n");
    test_memory_create();
    test_memory_append();
    test_memory_replace();
    test_memory_read_only();
    test_memory_char_limit();
    printf("ALL PASSED\n");
    return 0;
}
