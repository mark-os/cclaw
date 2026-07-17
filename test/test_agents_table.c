#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "test_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>

#define TEST_DB "/tmp/test_agents_table.sqlite"
#define AGENTS_DIR "/tmp/test_agents_t119"

static sqlite3 *setup(void) {
    test_db_clean(TEST_DB);
    return test_db_open(TEST_DB);
}

static void teardown(sqlite3 *db) {
    db_close(db);
    test_db_clean(TEST_DB);
}

/* agents table exists and basic CRUD works */
static void test_upsert_and_get(void) {
    sqlite3 *db = setup();

    int rc = db_agent_upsert(db, "coder", NULL, "You are a coder.");
    assert(rc == 0);

    AgentRow *row = db_agent_get(db, "coder");
    assert(row != NULL);
    assert(strcmp(row->name, "coder") == 0);
    /* config column removed */
    assert(strcmp(row->system_prompt, "You are a coder.") == 0);
    /* heartbeat column removed */
    /* id column removed from new schema */
    agent_row_free(row);

    /* Update existing */
    rc = db_agent_upsert(db, "coder", NULL, "Updated prompt.");
    assert(rc == 0);

    row = db_agent_get(db, "coder");
    assert(row != NULL);
    /* config column removed */
    assert(strcmp(row->system_prompt, "Updated prompt.") == 0);
    /* heartbeat column removed */
    agent_row_free(row);

    teardown(db);
    printf("  PASS test_upsert_and_get\n");
}

/* get returns NULL for nonexistent agent */
static void test_get_missing(void) {
    sqlite3 *db = setup();
    AgentRow *row = db_agent_get(db, "nonexistent");
    assert(row == NULL);
    teardown(db);
    printf("  PASS test_get_missing\n");
}

/* seed from disk on first reference, DB authoritative after */
static void test_seed_from_disk(void) {
    /* Create agent dir with files */
    mkdir(AGENTS_DIR, 0755);
    mkdir(AGENTS_DIR "/Mybot", 0755);

    FILE *f = fopen(AGENTS_DIR "/Mybot/agent.json", "w");
    fprintf(f, "{\"model\":\"deepseek\",\"max_iterations\":10}");
    fclose(f);

    f = fopen(AGENTS_DIR "/Mybot/system.md", "w");
    fprintf(f, "You are Mybot.");
    fclose(f);

    sqlite3 *db = setup();

    /* First call seeds from disk */
    AgentRow *row = db_agent_seed(db, AGENTS_DIR, "Mybot");
    assert(row != NULL);
    assert(strcmp(row->name, "Mybot") == 0);
    /* config column removed */
    assert(strcmp(row->system_prompt, "You are Mybot.") == 0);
    agent_row_free(row);

    /* Modify DB directly — DB is authoritative */
    db_agent_upsert(db, "Mybot", NULL, "DB prompt.");

    /* Second call returns DB version, not disk */
    row = db_agent_seed(db, AGENTS_DIR, "Mybot");
    assert(row != NULL);
    /* config column removed */
    assert(strcmp(row->system_prompt, "DB prompt.") == 0);
    agent_row_free(row);

    teardown(db);
    unlink(AGENTS_DIR "/Mybot/agent.json");
    unlink(AGENTS_DIR "/Mybot/system.md");
    rmdir(AGENTS_DIR "/Mybot");
    rmdir(AGENTS_DIR);
    printf("  PASS test_seed_from_disk\n");
}

/* seed with no agents_dir still creates row */
static void test_seed_no_dir(void) {
    sqlite3 *db = setup();
    AgentRow *row = db_agent_seed(db, "/nonexistent/path", "Ghost");
    assert(row != NULL);
    assert(strcmp(row->name, "Ghost") == 0);
    assert(row->description == NULL);
    assert(row->system_prompt == NULL);
    agent_row_free(row);
    teardown(db);
    printf("  PASS test_seed_no_dir\n");
}

int main(void) {
    TEST_INIT();
    printf("test_agents_table:\n");
    test_upsert_and_get();
    test_get_missing();
    test_seed_from_disk();
    test_seed_no_dir();
    printf("All agents table tests passed.\n");
    return 0;
}
