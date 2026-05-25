#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_DB "/tmp/test_soul_migration.sqlite"

/* T155: verify migration of soul/memory columns to memory_blocks */
static void test_migration_from_old_schema(void) {
    unlink(TEST_DB);

    /* Create DB with old schema (includes soul/memory columns) */
    sqlite3 *db = NULL;
    assert(sqlite3_open(TEST_DB, &db) == SQLITE_OK);
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA busy_timeout=5000;", NULL, NULL, NULL);

    /* Create old-style agents table with soul/memory columns */
    const char *old_schema =
        "CREATE TABLE agents ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  name TEXT NOT NULL UNIQUE,"
        "  config TEXT,"
        "  system_prompt TEXT,"
        "  soul TEXT,"
        "  memory TEXT,"
        "  heartbeat TEXT,"
        "  created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
        "  updated_at INTEGER NOT NULL DEFAULT (unixepoch())"
        ");"
        "CREATE TABLE memory_blocks ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  agent_name TEXT NOT NULL,"
        "  label TEXT NOT NULL,"
        "  value TEXT DEFAULT '',"
        "  description TEXT DEFAULT '',"
        "  char_limit INTEGER DEFAULT 5000,"
        "  read_only INTEGER DEFAULT 0,"
        "  created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
        "  updated_at INTEGER NOT NULL DEFAULT (unixepoch()),"
        "  UNIQUE(agent_name, label)"
        ");";
    char *err = NULL;
    assert(sqlite3_exec(db, old_schema, NULL, NULL, &err) == SQLITE_OK);

    /* Insert agent with soul and memory content */
    const char *insert = "INSERT INTO agents (name, config, system_prompt, soul, memory) "
                         "VALUES ('bot1', '{\"model\":\"test\"}', 'You are bot1.', "
                         "'I am friendly and helpful.', 'User prefers C.');";
    assert(sqlite3_exec(db, insert, NULL, NULL, &err) == SQLITE_OK);

    /* Also insert agent with NULL soul/memory (should not create blocks) */
    const char *insert2 = "INSERT INTO agents (name) VALUES ('bot2');";
    assert(sqlite3_exec(db, insert2, NULL, NULL, &err) == SQLITE_OK);

    sqlite3_close(db);

    /* Re-open with db_open — triggers migration */
    db = db_open(TEST_DB);
    assert(db != NULL);

    /* Verify soul migrated to persona block */
    MemoryBlock *mb = memory_block_get(db, "bot1", "persona");
    assert(mb != NULL);
    assert(strcmp(mb->value, "I am friendly and helpful.") == 0);
    assert(strcmp(mb->description, "Agent identity and tone") == 0);
    memory_block_free(mb);

    /* Verify memory migrated to human block */
    mb = memory_block_get(db, "bot1", "human");
    assert(mb != NULL);
    assert(strcmp(mb->value, "User prefers C.") == 0);
    assert(strcmp(mb->description, "Known facts about the user") == 0);
    memory_block_free(mb);

    /* Verify bot2 has no blocks (NULL soul/memory) */
    mb = memory_block_get(db, "bot2", "persona");
    assert(mb == NULL);
    mb = memory_block_get(db, "bot2", "human");
    assert(mb == NULL);

    /* Verify soul/memory columns are gone */
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, "SELECT soul FROM agents", -1, &stmt, NULL);
    assert(rc != SQLITE_OK); /* column should not exist */

    /* Verify agent row still works */
    AgentRow *row = db_agent_get(db, "bot1");
    assert(row != NULL);
    assert(strcmp(row->name, "bot1") == 0);
    assert(strcmp(row->system_prompt, "You are bot1.") == 0);
    agent_row_free(row);

    db_close(db);
    unlink(TEST_DB);
    printf("  PASS test_migration_from_old_schema\n");
}

/* T155: fresh DB has no soul/memory columns */
static void test_fresh_db_no_columns(void) {
    unlink(TEST_DB);
    sqlite3 *db = db_open(TEST_DB);
    assert(db != NULL);

    /* Verify soul/memory columns don't exist */
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, "SELECT soul FROM agents", -1, &stmt, NULL);
    assert(rc != SQLITE_OK);

    /* Agent CRUD works without soul/memory */
    assert(db_agent_upsert(db, "test", "{}", "prompt", NULL) == 0);
    AgentRow *row = db_agent_get(db, "test");
    assert(row != NULL);
    assert(strcmp(row->system_prompt, "prompt") == 0);
    agent_row_free(row);

    db_close(db);
    unlink(TEST_DB);
    printf("  PASS test_fresh_db_no_columns\n");
}

int main(void) {
    printf("test_soul_memory_migration:\n");
    test_migration_from_old_schema();
    test_fresh_db_no_columns();
    printf("All soul/memory migration tests passed.\n");
    return 0;
}
