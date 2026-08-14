#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "test_util.h"
#include "tool_memory.h"
#include "tools.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Memory tools: numbered-entry model (memory_create/add/edit/delete). */

static sqlite3 *setup(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    test_seed_agent(db, "bot");
    return db;
}

static void test_create_and_add(void) {
    sqlite3 *db = setup();
    ToolRegistry reg;
    tools_init(&reg);
    ToolMemoryCtx ctx = {.db = db, .agent_name = "bot"};
    tool_memory_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "memory_create");
    assert(e);
    char *r = e->handler("{\"label\":\"facts\",\"description\":\"user facts\"}", e->user_data, &(int){0});
    assert(strstr(r, "ok"));
    free(r);

    e = tools_lookup(&reg, "memory_add");
    assert(e);
    r = e->handler("{\"block\":\"facts\",\"text\":\"likes coffee\"}", e->user_data, &(int){0});
    /* result echoes the rendered block including the new entry */
    assert(strstr(r, "likes coffee"));
    free(r);

    int n = 0;
    MemoryEntry *entries = memory_entries_list(db, "bot", "facts", &n);
    assert(n == 1);
    assert(entries[0].pos == 1);
    assert(strcmp(entries[0].text, "likes coffee") == 0);
    memory_entries_free(entries, n);

    tools_free(&reg);
    db_close(db);
    printf("PASS: test_create_and_add\n");
}

static void test_edit_batch(void) {
    sqlite3 *db = setup();
    ToolRegistry reg;
    tools_init(&reg);
    ToolMemoryCtx ctx = {.db = db, .agent_name = "bot"};
    tool_memory_register(&reg, &ctx);

    ToolEntry *create = tools_lookup(&reg, "memory_create");
    char *r = create->handler("{\"label\":\"bio\",\"description\":\"about\"}", create->user_data, &(int){0});
    assert(strstr(r, "ok")); free(r);

    ToolEntry *add = tools_lookup(&reg, "memory_add");
    r = add->handler("{\"block\":\"bio\",\"text\":\"age 30\"}", add->user_data, &(int){0}); free(r);
    r = add->handler("{\"block\":\"bio\",\"text\":\"city NYC\"}", add->user_data, &(int){0}); free(r);

    /* Batch edit both entries by number */
    ToolEntry *edit = tools_lookup(&reg, "memory_edit");
    r = edit->handler(
        "{\"block\":\"bio\",\"edits\":[{\"number\":1,\"text\":\"age 31\"},"
        "{\"number\":2,\"text\":\"city LA\"}]}", edit->user_data, &(int){0});
    assert(strstr(r, "edited 2"));
    free(r);

    int n = 0;
    MemoryEntry *entries = memory_entries_list(db, "bot", "bio", &n);
    assert(n == 2);
    assert(strcmp(entries[0].text, "age 31") == 0);
    assert(strcmp(entries[1].text, "city LA") == 0);
    memory_entries_free(entries, n);

    tools_free(&reg);
    db_close(db);
    printf("PASS: test_edit_batch\n");
}

static void test_delete_renumber(void) {
    sqlite3 *db = setup();
    ToolRegistry reg;
    tools_init(&reg);
    ToolMemoryCtx ctx = {.db = db, .agent_name = "bot"};
    tool_memory_register(&reg, &ctx);

    ToolEntry *create = tools_lookup(&reg, "memory_create");
    char *r = create->handler("{\"label\":\"log\",\"description\":\"events\"}", create->user_data, &(int){0});
    assert(strstr(r, "ok")); free(r);

    ToolEntry *add = tools_lookup(&reg, "memory_add");
    r = add->handler("{\"block\":\"log\",\"text\":\"one\"}", add->user_data, &(int){0}); free(r);
    r = add->handler("{\"block\":\"log\",\"text\":\"two\"}", add->user_data, &(int){0}); free(r);
    r = add->handler("{\"block\":\"log\",\"text\":\"three\"}", add->user_data, &(int){0}); free(r);

    /* Delete the middle entry; survivors must renumber to 1,2 */
    ToolEntry *del = tools_lookup(&reg, "memory_delete");
    r = del->handler("{\"block\":\"log\",\"numbers\":[2]}", del->user_data, &(int){0});
    assert(strstr(r, "deleted 1"));
    free(r);

    int n = 0;
    MemoryEntry *entries = memory_entries_list(db, "bot", "log", &n);
    assert(n == 2);
    assert(entries[0].pos == 1 && strcmp(entries[0].text, "one") == 0);
    assert(entries[1].pos == 2 && strcmp(entries[1].text, "three") == 0);
    memory_entries_free(entries, n);

    tools_free(&reg);
    db_close(db);
    printf("PASS: test_delete_renumber\n");
}

static void test_missing_fields(void) {
    sqlite3 *db = setup();
    ToolRegistry reg;
    tools_init(&reg);
    ToolMemoryCtx ctx = {.db = db, .agent_name = "bot"};
    tool_memory_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "memory_create");
    char *r = e->handler("{}", e->user_data, &(int){0});
    assert(strstr(r, "error"));
    free(r);

    e = tools_lookup(&reg, "memory_add");
    r = e->handler("{\"block\":\"x\"}", e->user_data, &(int){0});
    assert(strstr(r, "error"));
    free(r);

    /* add to nonexistent block */
    r = e->handler("{\"block\":\"nope\",\"text\":\"y\"}", e->user_data, &(int){0});
    assert(strstr(r, "error"));
    free(r);

    tools_free(&reg);
    db_close(db);
    printf("PASS: test_missing_fields\n");
}

static void test_memory_create(void) {
    sqlite3 *db = setup();
    ToolMemoryCtx ctx = {.db = db, .agent_name = "bot"};
    ToolRegistry reg;
    tools_init(&reg);
    tool_memory_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "memory_create");
    assert(e);

    char *r = e->handler("{\"label\":\"notes\",\"description\":\"scratch pad\"}", e->user_data, &(int){0});
    assert(strstr(r, "ok"));
    free(r);

    /* Block exists with the given description; starts with no entries */
    MemoryBlock *mb = memory_block_get(db, "bot", "notes");
    assert(mb);
    assert(strcmp(mb->description, "scratch pad") == 0);
    memory_block_free(mb);

    int n = 0;
    MemoryEntry *entries = memory_entries_list(db, "bot", "notes", &n);
    assert(n == 0 && entries == NULL);

    /* Duplicate label fails */
    r = e->handler("{\"label\":\"notes\",\"description\":\"dup\"}", e->user_data, &(int){0});
    assert(strstr(r, "error"));
    free(r);

    tools_free(&reg);
    db_close(db);
    printf("PASS: test_memory_create\n");
}

static void test_memory_read_only(void) {
    sqlite3 *db = setup();
    ToolMemoryCtx ctx = {.db = db, .agent_name = "bot"};
    ToolRegistry reg;
    tools_init(&reg);
    tool_memory_register(&reg, &ctx);

    ToolEntry *create = tools_lookup(&reg, "memory_create");
    char *r = create->handler("{\"label\":\"locked\",\"description\":\"immutable\"}", create->user_data, &(int){0});
    assert(strstr(r, "ok"));
    free(r);

    /* Seed one entry, then lock the block */
    memory_entry_add(db, "bot", "locked", "fixed");
    sqlite3_exec(db, "UPDATE memory_blocks SET read_only=1 WHERE label='locked';", NULL, NULL, NULL);

    /* add / edit / delete all rejected */
    ToolEntry *add = tools_lookup(&reg, "memory_add");
    r = add->handler("{\"block\":\"locked\",\"text\":\"more\"}", add->user_data, &(int){0});
    assert(strstr(r, "read-only"));
    free(r);

    ToolEntry *edit = tools_lookup(&reg, "memory_edit");
    r = edit->handler("{\"block\":\"locked\",\"edits\":[{\"number\":1,\"text\":\"changed\"}]}", edit->user_data, &(int){0});
    assert(strstr(r, "read-only"));
    free(r);

    ToolEntry *del = tools_lookup(&reg, "memory_delete");
    r = del->handler("{\"block\":\"locked\",\"numbers\":[1]}", del->user_data, &(int){0});
    assert(strstr(r, "read-only"));
    free(r);

    /* Entry unchanged */
    int n = 0;
    MemoryEntry *entries = memory_entries_list(db, "bot", "locked", &n);
    assert(n == 1 && strcmp(entries[0].text, "fixed") == 0);
    memory_entries_free(entries, n);

    tools_free(&reg);
    db_close(db);
    printf("PASS: test_memory_read_only\n");
}

static void test_memory_char_limit(void) {
    sqlite3 *db = setup();
    ToolMemoryCtx ctx = {.db = db, .agent_name = "bot"};
    ToolRegistry reg;
    tools_init(&reg);
    tool_memory_register(&reg, &ctx);

    /* Create a block with a small char_limit and one existing entry */
    memory_block_create(db, "bot", "tiny", "small block", 5, NULL);
    memory_entry_add(db, "bot", "tiny", "hi"); /* 2 chars */

    /* Adding 4 more chars (total 6 > 5) must be rejected */
    ToolEntry *add = tools_lookup(&reg, "memory_add");
    char *r = add->handler("{\"block\":\"tiny\",\"text\":\"1234\"}", add->user_data, &(int){0});
    assert(strstr(r, "exceed"));
    free(r);

    tools_free(&reg);
    db_close(db);
    printf("PASS: test_memory_char_limit\n");
}

int main(void) {
    TEST_INIT();
    test_create_and_add();
    test_edit_batch();
    test_delete_renumber();
    test_missing_fields();
    test_memory_create();
    test_memory_read_only();
    test_memory_char_limit();
    printf("All memory tool tests passed.\n");
    return 0;
}
