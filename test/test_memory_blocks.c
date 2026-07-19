#include "db.h"
#include "test_util.h"
#include "agent_config.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define TEST_DB "/tmp/test_cclaw_memblocks.sqlite"

/* memory_blocks/memory_entries.agent_name are FKs (v31) — seed every agent
 * name this file writes child rows for before any test body runs. */
static sqlite3 *open_seeded(void) {
    sqlite3 *db = test_db_open(TEST_DB);
    assert(db);
    test_seed_agent(db, "TestAgent");
    test_seed_agent(db, "OtherAgent");
    test_seed_agent(db, "Agent1");
    test_seed_agent(db, "Agent2");
    test_seed_agent(db, "Agent");
    test_seed_agent(db, "Myagent");
    test_seed_agent(db, "Testagent");
    return db;
}

static void test_create_and_get(void) {
    sqlite3 *db = open_seeded();

    int64_t id = memory_block_create(db, "TestAgent", "persona",
                                     "Agent identity", "I am helpful", 5000, NULL);
    assert(id > 0);

    MemoryBlock *mb = memory_block_get(db, "TestAgent", "persona");
    assert(mb);
    assert(strcmp(mb->agent_name, "TestAgent") == 0);
    assert(strcmp(mb->label, "persona") == 0);
    assert(strcmp(mb->value, "I am helpful") == 0);
    assert(strcmp(mb->description, "Agent identity") == 0);
    assert(mb->char_limit == 5000);
    assert(mb->read_only == 0);
    /* NULL placement defaults to 'context' — new memory rides in the
     * per-turn session context block, not the system prompt. */
    assert(strcmp(mb->placement, "context") == 0);
    memory_block_free(mb);

    /* Duplicate label fails */
    int64_t dup = memory_block_create(db, "TestAgent", "persona", "dup", "dup", 5000, NULL);
    assert(dup == -1);

    /* Different agent same label OK */
    int64_t id2 = memory_block_create(db, "OtherAgent", "persona", "desc", "val", 3000, NULL);
    assert(id2 > 0);

    db_close(db);
    test_db_clean(TEST_DB);
    printf("  PASS test_create_and_get\n");
}

static void test_seed(void) {
    sqlite3 *db = open_seeded();

    const char *json =
        "{\"memory_blocks\":["
        "{\"label\":\"persona\",\"description\":\"identity\",\"value\":\"I am X\",\"char_limit\":3000,\"read_only\":true},"
        "{\"label\":\"human\",\"description\":\"user facts\",\"char_limit\":5000}"
        "]}";

    memory_blocks_seed(db, "Myagent", json);

    MemoryBlock *mb = memory_block_get(db, "Myagent", "persona");
    assert(mb);
    assert(strcmp(mb->value, "I am X") == 0);
    assert(mb->char_limit == 3000);
    assert(mb->read_only == 1);
    memory_block_free(mb);

    mb = memory_block_get(db, "Myagent", "human");
    assert(mb);
    assert(strcmp(mb->value, "") == 0);
    assert(mb->char_limit == 5000);
    assert(mb->read_only == 0);
    assert(strcmp(mb->placement, "context") == 0);  /* unspecified → context */
    memory_block_free(mb);

    /* Re-seed doesn't overwrite (DB authoritative) */
    sqlite3_exec(db, "UPDATE memory_blocks SET value='likes cats' WHERE agent_name='Myagent' AND label='human';", NULL, NULL, NULL);
    memory_blocks_seed(db, "Myagent", json);
    mb = memory_block_get(db, "Myagent", "human");
    assert(strcmp(mb->value, "likes cats") == 0);
    memory_block_free(mb);

    db_close(db);
    test_db_clean(TEST_DB);
    printf("  PASS test_seed\n");
}

static void test_prompt_injection(void) {
    sqlite3 *db = open_seeded();

    /* Seed agent with system prompt */
    db_agent_upsert(db, "Testagent", NULL, "You are a test agent.");

    /* Create memory blocks (containers) and add numbered entries. System
     * placement — this test covers system-prompt rendering; context-placement
     * rendering is covered in test_llm_payload. */
    memory_block_create(db, "Testagent", "persona", "Agent identity", "", 5000, "system");
    memory_block_create(db, "Testagent", "human", "User facts", "", 3000, "system");
    memory_entry_add(db, "Testagent", "persona", "I am helpful"); /* 12 chars */
    memory_entry_add(db, "Testagent", "human", "Likes cats");     /* 10 chars */

    /* Build system prompt — no agents_dir needed, agent already in DB */
    Config cfg = {0};
    cfg.context_window = 128000;
    char *prompt = agent_build_system_prompt(db, "Testagent", 1, NULL, &cfg);
    assert(prompt);

    /* Verify memory blocks render as numbered entries */
    assert(strstr(prompt, "## Memory Blocks"));
    assert(strstr(prompt, "### persona"));
    assert(strstr(prompt, "description: Agent identity"));
    assert(strstr(prompt, "usage: 12/5000 chars"));
    assert(strstr(prompt, "1. I am helpful"));
    assert(strstr(prompt, "### human"));
    assert(strstr(prompt, "description: User facts"));
    assert(strstr(prompt, "usage: 10/3000 chars"));
    assert(strstr(prompt, "1. Likes cats"));
    assert(strstr(prompt, "read-write"));

    free(prompt);
    db_close(db);
    test_db_clean(TEST_DB);
    printf("  PASS test_prompt_injection\n");
}

int main(void) {
    TEST_INIT();
    printf("test_memory_blocks:\n");
    test_create_and_get();
    test_seed();
    test_prompt_injection();
    printf("All memory_blocks tests passed.\n");
    return 0;
}
