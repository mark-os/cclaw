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

/* memory_blocks.agent_name / agent_memory_blocks.agent_name are FKs — seed every agent
 * name this file writes child rows for before any test body runs. */
static int64_t bid(sqlite3 *db, const char *agent, const char *label) {
    MemoryBlock *mb = memory_block_get(db, agent, label);
    assert(mb);
    int64_t id = mb->id;
    memory_block_free(mb);
    return id;
}

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
                                     "Agent identity", 5000, NULL);
    assert(id > 0);

    MemoryBlock *mb = memory_block_get(db, "TestAgent", "persona");
    assert(mb);
    assert(strcmp(mb->agent_name, "TestAgent") == 0);
    assert(strcmp(mb->label, "persona") == 0);
    assert(strcmp(mb->description, "Agent identity") == 0);
    assert(mb->char_limit == 5000);
    assert(mb->read_only == 0);
    /* NULL placement defaults to 'context' — new memory rides in the
     * per-turn session context block, not the system prompt. */
    assert(strcmp(mb->placement, "context") == 0);
    memory_block_free(mb);

    /* Duplicate label fails */
    int64_t dup = memory_block_create(db, "TestAgent", "persona", "dup", 5000, NULL);
    assert(dup == -1);

    /* Different agent same label OK */
    int64_t id2 = memory_block_create(db, "OtherAgent", "persona", "desc", 3000, NULL);
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
    assert(mb->char_limit == 3000);
    assert(mb->read_only == 1);
    memory_block_free(mb);

    /* A manifest $.value lands as entry 1 of the block — blocks hold no
     * scalar text, so this is the only way seeded content is ever visible. */
    int n = 0;
    MemoryNote *entries = memory_notes_list(db, bid(db, "Myagent", "persona"), &n);
    assert(n == 1);
    assert(strcmp(entries[0].text, "I am X") == 0);
    memory_notes_free(entries, n);

    mb = memory_block_get(db, "Myagent", "human");
    assert(mb);
    assert(mb->char_limit == 5000);
    assert(mb->read_only == 0);
    assert(strcmp(mb->placement, "context") == 0);  /* unspecified → context */
    memory_block_free(mb);

    /* No $.value → an empty container, not a blank entry. */
    entries = memory_notes_list(db, bid(db, "Myagent", "human"), &n);
    assert(n == 0 && entries == NULL);

    /* Re-seed doesn't overwrite (DB authoritative): existing entries survive
     * and the seed adds none. */
    assert(memory_note_add(db, bid(db, "Myagent", "human"), "likes cats") > 0);
    memory_blocks_seed(db, "Myagent", json);
    entries = memory_notes_list(db, bid(db, "Myagent", "human"), &n);
    assert(n == 1);
    assert(strcmp(entries[0].text, "likes cats") == 0);
    memory_notes_free(entries, n);

    /* {"attach": id} shares an existing block instead of creating one:
     * read-only by default, read-write only when the definition says so. */
    int64_t shared = bid(db, "Myagent", "human");
    char def[160];
    snprintf(def, sizeof(def), "{\"memory_blocks\":[{\"attach\":%lld}]}", (long long)shared);
    memory_blocks_seed(db, "Agent1", def);
    mb = memory_block_get(db, "Agent1", "human");
    assert(mb && mb->id == shared && mb->read_only == 1);
    memory_block_free(mb);
    snprintf(def, sizeof(def),
             "{\"memory_blocks\":[{\"attach\":%lld,\"read_only\":false}]}", (long long)shared);
    memory_blocks_seed(db, "Agent2", def);
    mb = memory_block_get(db, "Agent2", "human");
    assert(mb && mb->read_only == 0);
    memory_block_free(mb);

    db_close(db);
    test_db_clean(TEST_DB);
    printf("  PASS test_seed\n");
}

static void test_prompt_injection(void) {
    sqlite3 *db = open_seeded();

    /* Seed agent with system prompt */
    db_agent_upsert(db, "Testagent", NULL, "You are a test agent.");

    /* Create memory blocks (containers) and add notes. System
     * placement — this test covers system-prompt rendering; context-placement
     * rendering is covered in test_llm_payload. */
    memory_block_create(db, "Testagent", "persona", "Agent identity", 5000, "system");
    memory_block_create(db, "Testagent", "human", "User facts", 3000, "system");
    int64_t n1 = memory_note_add(db, bid(db, "Testagent", "persona"), "I am helpful"); /* 12 chars */
    int64_t n2 = memory_note_add(db, bid(db, "Testagent", "human"), "Likes cats");     /* 10 chars */
    assert(n1 > 0 && n2 > 0);
    /* A block shared from another agent renders too, marked as such. */
    int64_t team = memory_block_create(db, "OtherAgent", "team", "Shared facts", 5000, "system");
    assert(memory_note_add(db, team, "standup at 9") > 0);
    assert(memory_block_attach(db, "Testagent", team, 1) == 0);

    /* Build system prompt — no agents_dir needed, agent already in DB */
    Config cfg = {0};
    cfg.context_window = 128000;
    char *prompt = agent_build_system_prompt(db, "Testagent", 1, NULL, &cfg);
    assert(prompt);

    /* Verify memory blocks render their notes with [id] handles */
    assert(strstr(prompt, "## Memory Blocks"));
    assert(strstr(prompt, "### persona"));
    assert(strstr(prompt, "description: Agent identity"));
    assert(strstr(prompt, "usage: 12/5000 chars | read-write | placement: system\n"));
    char want[64];
    /* "[id date] text" — the date is when the note last changed. */
    sqlite3_exec(db, "UPDATE memory_notes SET updated_at=unixepoch('2026-03-04 12:00:00')"
                     " WHERE text='I am helpful'", NULL, NULL, NULL);
    free(prompt);
    prompt = agent_build_system_prompt(db, "Testagent", 1, NULL, &cfg);
    snprintf(want, sizeof(want), "[%lld 2026-03-04] I am helpful", (long long)n1);
    assert(strstr(prompt, want));
    assert(strstr(prompt, "### human"));
    assert(strstr(prompt, "description: User facts"));
    assert(strstr(prompt, "usage: 10/3000 chars"));
    snprintf(want, sizeof(want), "[%lld 20", (long long)n2);
    assert(strstr(prompt, want));
    assert(strstr(prompt, "] Likes cats"));
    assert(strstr(prompt, "### team"));
    assert(strstr(prompt, "usage: 12/5000 chars | read-only | placement: system | shared\n"));
    assert(strstr(prompt, "] standup at 9"));
    /* The owner's own view of that block is not marked shared-in. */
    char *owner_prompt = agent_build_system_prompt(db, "OtherAgent", 1, NULL, &cfg);
    assert(owner_prompt && strstr(owner_prompt, "### team"));
    assert(strstr(owner_prompt, "| read-write | placement: system\n"));
    assert(!strstr(owner_prompt, "| shared"));
    free(owner_prompt);

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
