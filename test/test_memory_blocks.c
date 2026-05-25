#include "db.h"
#include "agent_config.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define TEST_DB "/tmp/test_cclaw_memblocks.sqlite"

static void test_create_and_get(void) {
    sqlite3 *db = db_open(TEST_DB);
    assert(db);

    int64_t id = memory_block_create(db, "test_agent", "persona",
                                     "Agent identity", "I am helpful", 5000);
    assert(id > 0);

    MemoryBlock *mb = memory_block_get(db, "test_agent", "persona");
    assert(mb);
    assert(strcmp(mb->agent_name, "test_agent") == 0);
    assert(strcmp(mb->label, "persona") == 0);
    assert(strcmp(mb->value, "I am helpful") == 0);
    assert(strcmp(mb->description, "Agent identity") == 0);
    assert(mb->char_limit == 5000);
    assert(mb->read_only == 0);
    memory_block_free(mb);

    /* Duplicate label fails */
    int64_t dup = memory_block_create(db, "test_agent", "persona", "dup", "dup", 5000);
    assert(dup == -1);

    /* Different agent same label OK */
    int64_t id2 = memory_block_create(db, "other_agent", "persona", "desc", "val", 3000);
    assert(id2 > 0);

    db_close(db);
    unlink(TEST_DB);
    printf("  PASS test_create_and_get\n");
}

static void test_list(void) {
    sqlite3 *db = db_open(TEST_DB);
    assert(db);

    memory_block_create(db, "agent1", "a", "d1", "v1", 5000);
    memory_block_create(db, "agent1", "b", "d2", "v2", 5000);
    memory_block_create(db, "agent2", "c", "d3", "v3", 5000);

    int count = 0;
    MemoryBlock *list = memory_block_list(db, "agent1", &count);
    assert(count == 2);
    assert(strcmp(list[0].label, "a") == 0);
    assert(strcmp(list[1].label, "b") == 0);
    memory_block_list_free(list, count);

    list = memory_block_list(db, "agent2", &count);
    assert(count == 1);
    memory_block_list_free(list, count);

    list = memory_block_list(db, "nonexistent", &count);
    assert(count == 0);
    assert(list == NULL);

    db_close(db);
    unlink(TEST_DB);
    printf("  PASS test_list\n");
}

static void test_set_value(void) {
    sqlite3 *db = db_open(TEST_DB);
    assert(db);

    memory_block_create(db, "agent", "notes", "my notes", "", 20);

    /* Normal update */
    int rc = memory_block_set_value(db, "agent", "notes", "short");
    assert(rc == 0);
    MemoryBlock *mb = memory_block_get(db, "agent", "notes");
    assert(strcmp(mb->value, "short") == 0);
    memory_block_free(mb);

    /* V55: exceeds char_limit → rejected */
    rc = memory_block_set_value(db, "agent", "notes", "this string is way too long for the limit");
    assert(rc == -1);

    /* Value unchanged after rejection */
    mb = memory_block_get(db, "agent", "notes");
    assert(strcmp(mb->value, "short") == 0);
    memory_block_free(mb);

    /* Nonexistent block → error */
    rc = memory_block_set_value(db, "agent", "nope", "val");
    assert(rc == -1);

    db_close(db);
    unlink(TEST_DB);
    printf("  PASS test_set_value\n");
}

static void test_read_only(void) {
    sqlite3 *db = db_open(TEST_DB);
    assert(db);

    int64_t id = memory_block_create(db, "agent", "instructions", "standing orders", "do X", 5000);
    assert(id > 0);

    /* Set read_only directly */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "UPDATE memory_blocks SET read_only=1 WHERE id=?;", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* V55: read_only blocks reject edits */
    int rc = memory_block_set_value(db, "agent", "instructions", "new value");
    assert(rc == -1);

    MemoryBlock *mb = memory_block_get(db, "agent", "instructions");
    assert(strcmp(mb->value, "do X") == 0);
    memory_block_free(mb);

    db_close(db);
    unlink(TEST_DB);
    printf("  PASS test_read_only\n");
}

static void test_seed(void) {
    sqlite3 *db = db_open(TEST_DB);
    assert(db);

    const char *json =
        "{\"memory_blocks\":["
        "{\"label\":\"persona\",\"description\":\"identity\",\"value\":\"I am X\",\"char_limit\":3000,\"read_only\":true},"
        "{\"label\":\"human\",\"description\":\"user facts\",\"char_limit\":5000}"
        "]}";

    memory_blocks_seed(db, "myagent", json);

    MemoryBlock *mb = memory_block_get(db, "myagent", "persona");
    assert(mb);
    assert(strcmp(mb->value, "I am X") == 0);
    assert(mb->char_limit == 3000);
    assert(mb->read_only == 1);
    memory_block_free(mb);

    mb = memory_block_get(db, "myagent", "human");
    assert(mb);
    assert(strcmp(mb->value, "") == 0);
    assert(mb->char_limit == 5000);
    assert(mb->read_only == 0);
    memory_block_free(mb);

    /* Re-seed doesn't overwrite (DB authoritative) */
    memory_block_set_value(db, "myagent", "human", "likes cats");
    memory_blocks_seed(db, "myagent", json);
    mb = memory_block_get(db, "myagent", "human");
    assert(strcmp(mb->value, "likes cats") == 0);
    memory_block_free(mb);

    db_close(db);
    unlink(TEST_DB);
    printf("  PASS test_seed\n");
}

static void test_prompt_injection(void) {
    sqlite3 *db = db_open(TEST_DB);
    assert(db);

    /* Seed agent with system prompt */
    db_agent_upsert(db, "testagent", "{}", "You are a test agent.", NULL);

    /* Create memory blocks */
    memory_block_create(db, "testagent", "persona", "Agent identity", "I am helpful", 5000);
    memory_block_create(db, "testagent", "human", "User facts", "Likes cats", 3000);

    /* Build system prompt — no agents_dir needed, agent already in DB */
    Config cfg = {0};
    cfg.provider.context_window = 128000;
    char *prompt = agent_build_system_prompt(db, "testagent", 1, NULL, &cfg);
    assert(prompt);

    /* Verify memory blocks section present */
    assert(strstr(prompt, "## Memory Blocks"));
    assert(strstr(prompt, "### persona"));
    assert(strstr(prompt, "description: Agent identity"));
    assert(strstr(prompt, "usage: 12/5000 chars"));
    assert(strstr(prompt, "I am helpful"));
    assert(strstr(prompt, "### human"));
    assert(strstr(prompt, "description: User facts"));
    assert(strstr(prompt, "usage: 10/3000 chars"));
    assert(strstr(prompt, "Likes cats"));
    assert(strstr(prompt, "read-write"));

    free(prompt);
    db_close(db);
    unlink(TEST_DB);
    printf("  PASS test_prompt_injection\n");
}

int main(void) {
    printf("test_memory_blocks:\n");
    test_create_and_get();
    test_list();
    test_set_value();
    test_read_only();
    test_seed();
    test_prompt_injection();
    printf("All memory_blocks tests passed.\n");
    return 0;
}
