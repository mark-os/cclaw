#include "agent_config.h"
#include "db.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DB_PATH "/tmp/test_ephemeral.db"

static int test_create_ephemeral(void) {
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    assert(db);

    char *name = agent_create_ephemeral("agents", db);
    assert(name);
    assert(strncmp(name, "ephemeral-", 10) == 0);
    assert(strlen(name) == 10 + 36);

    /* Verify seeded in DB */
    AgentRow *row = db_agent_get(db, name);
    assert(row != NULL);
    assert(strcmp(row->name, name) == 0);
    agent_row_free(row);

    /* Two calls get different names */
    char *name2 = agent_create_ephemeral("agents", db);
    assert(name2 && strcmp(name, name2) != 0);

    free(name2);
    free(name);
    db_close(db);
    unlink(DB_PATH);
    printf("PASS: test_create_ephemeral\n");
    return 0;
}

int main(void) {
    if (test_create_ephemeral()) return 1;
    printf("All ephemeral agent tests passed.\n");
    return 0;
}
