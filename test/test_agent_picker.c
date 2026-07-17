/* Unit test for CLI agent picker DB operations */
#include "db.h"
#include "config_registry.h"
#include "test_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

static void test_agent_list_empty(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);

    int count = 0;
    char **agents = db_agent_list(db, &count);
    assert(agents == NULL);
    assert(count == 0);

    db_close(db);
    printf("  PASS test_agent_list_empty\n");
}

static void test_agent_list_one(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);

    int rc = db_agent_upsert(db, "default", NULL, NULL);
    assert(rc == 0);

    int count = 0;
    char **agents = db_agent_list(db, &count);
    assert(agents != NULL);
    assert(count == 1);
    assert(strcmp(agents[0], "default") == 0);

    free(agents[0]);
    free(agents);
    db_close(db);
    printf("  PASS test_agent_list_one\n");
}

static void test_agent_list_multiple_sorted(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);

    db_agent_upsert(db, "zebra", NULL, NULL);
    db_agent_upsert(db, "alpha", NULL, NULL);
    db_agent_upsert(db, "middle", NULL, NULL);

    int count = 0;
    char **agents = db_agent_list(db, &count);
    assert(agents != NULL);
    assert(count == 3);
    /* Sorted alphabetically */
    assert(strcmp(agents[0], "alpha") == 0);
    assert(strcmp(agents[1], "middle") == 0);
    assert(strcmp(agents[2], "zebra") == 0);

    for (int i = 0; i < count; i++) free(agents[i]);
    free(agents);
    db_close(db);
    printf("  PASS test_agent_list_multiple_sorted\n");
}

static void test_default_agent_kv(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);

    /* First run: no agents, set default_agent */
    int count = 0;
    char **agents = db_agent_list(db, &count);
    assert(!agents && count == 0);

    db_agent_upsert(db, "default", NULL, NULL);
    config_set(db, "default_agent", "default");

    char *def = config_get(db, "default_agent");
    assert(def);
    assert(strcmp(def, "default") == 0);
    free(def);

    /* Verify agent now listed */
    agents = db_agent_list(db, &count);
    assert(agents && count == 1);
    free(agents[0]);
    free(agents);

    db_close(db);
    printf("  PASS test_default_agent_kv\n");
}

int main(void) {
    TEST_INIT();
    printf("test_agent_picker:\n");
    test_agent_list_empty();
    test_agent_list_one();
    test_agent_list_multiple_sorted();
    test_default_agent_kv();
    printf("\nAll agent picker tests passed.\n");
    return 0;
}
