#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "test_util.h"
#include "agent_config.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>

#define TEST_DB "/tmp/test_session_agent.sqlite"
#define FAIL(msg) do { fprintf(stderr, "  FAIL %s: %s\n", __func__, msg); return; } while(0)

static sqlite3 *setup(void) {
    test_db_clean(TEST_DB);
    sqlite3 *db = test_db_open(TEST_DB);
    test_seed_agent(db, "coder");
    test_seed_agent(db, "writer");
    return db;
}

static void teardown(sqlite3 *db) {
    db_close(db);
    test_db_clean(TEST_DB);
}

/* session_create stores agent_name, session_get_agent_name retrieves it */
static void test_create_with_agent_name(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "test", "coder", -1, 0);
    assert(sid > 0);

    char *name = session_get_agent_name(db, sid);
    assert(name != NULL);
    assert(strcmp(name, "coder") == 0);
    free(name);

    teardown(db);
    printf("  PASS test_create_with_agent_name\n");
}

/* NULL agent_name → session_get_agent_name returns NULL */
static void test_create_without_agent_name(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    assert(sid > 0);

    char *name = session_get_agent_name(db, sid);
    assert(name == NULL);

    teardown(db);
    printf("  PASS test_create_without_agent_name\n");
}

int main(void) {
    TEST_INIT();
    printf("test_session_agent:\n");
    test_create_with_agent_name();
    test_create_without_agent_name();
    printf("All session↔agent binding tests passed.\n");
    return 0;
}
