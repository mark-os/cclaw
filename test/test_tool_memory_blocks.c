#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "db.h"
#include "test_util.h"
#include "tool_memory.h"
#include "tools.h"

#define FAIL(msg) do { fprintf(stderr, "FAIL: %s\n", msg); exit(1); } while(0)

static sqlite3 *setup_db(void) {
    sqlite3 *db = test_db_open(":memory:");
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

    char *r = e->handler("{\"label\":\"notes\",\"description\":\"scratch pad\"}", e->user_data);
    assert(strstr(r, "ok"));
    free(r);

    /* Block exists with the given description; starts with no entries */
    MemoryBlock *mb = memory_block_get(db, "test_agent", "notes");
    assert(mb);
    assert(strcmp(mb->description, "scratch pad") == 0);
    memory_block_free(mb);

    int n = 0;
    MemoryEntry *entries = memory_entries_list(db, "test_agent", "notes", &n);
    assert(n == 0 && entries == NULL);

    /* Duplicate label fails */
    r = e->handler("{\"label\":\"notes\",\"description\":\"dup\"}", e->user_data);
    assert(strstr(r, "error"));
    free(r);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_memory_create\n");
}

static void test_memory_read_only(void) {
    sqlite3 *db = setup_db();
    ToolMemoryCtx ctx = {.db = db, .agent_name = "test_agent"};
    ToolRegistry reg;
    tools_init(&reg);
    tool_memory_register(&reg, &ctx);

    ToolEntry *create = tools_lookup(&reg, "memory_create");
    char *r = create->handler("{\"label\":\"locked\",\"description\":\"immutable\"}", create->user_data);
    assert(strstr(r, "ok"));
    free(r);

    /* Seed one entry, then lock the block */
    memory_entry_add(db, "test_agent", "locked", "fixed");
    sqlite3_exec(db, "UPDATE memory_blocks SET read_only=1 WHERE label='locked';", NULL, NULL, NULL);

    /* add / edit / delete all rejected */
    ToolEntry *add = tools_lookup(&reg, "memory_add");
    r = add->handler("{\"block\":\"locked\",\"text\":\"more\"}", add->user_data);
    assert(strstr(r, "read-only"));
    free(r);

    ToolEntry *edit = tools_lookup(&reg, "memory_edit");
    r = edit->handler("{\"block\":\"locked\",\"edits\":[{\"number\":1,\"text\":\"changed\"}]}", edit->user_data);
    assert(strstr(r, "read-only"));
    free(r);

    ToolEntry *del = tools_lookup(&reg, "memory_delete");
    r = del->handler("{\"block\":\"locked\",\"numbers\":[1]}", del->user_data);
    assert(strstr(r, "read-only"));
    free(r);

    /* Entry unchanged */
    int n = 0;
    MemoryEntry *entries = memory_entries_list(db, "test_agent", "locked", &n);
    assert(n == 1 && strcmp(entries[0].text, "fixed") == 0);
    memory_entries_free(entries, n);

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

    /* Create a block with a small char_limit and one existing entry */
    memory_block_create(db, "test_agent", "tiny", "small block", "", 5);
    memory_entry_add(db, "test_agent", "tiny", "hi"); /* 2 chars */

    /* Adding 4 more chars (total 6 > 5) must be rejected */
    ToolEntry *add = tools_lookup(&reg, "memory_add");
    char *r = add->handler("{\"block\":\"tiny\",\"text\":\"1234\"}", add->user_data);
    assert(strstr(r, "exceed"));
    free(r);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_memory_char_limit\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_tool_memory_blocks:\n");
    test_memory_create();
    test_memory_read_only();
    test_memory_char_limit();
    printf("ALL PASSED\n");
    return 0;
}
