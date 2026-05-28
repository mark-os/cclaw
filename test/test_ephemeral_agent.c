#include "agent_config.h"
#include "db.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AGENTS_DIR "/tmp/test_ephemeral_agents"
#define DB_PATH "/tmp/test_ephemeral_agents.db"

#define FAIL(msg) do { fprintf(stderr, "FAIL: %s\n", msg); return 1; } while(0)

static void cleanup(void) {
    system("rm -rf " AGENTS_DIR);
    unlink(DB_PATH);
    unlink("/tmp/test_ephemeral_agents.db-wal");
    unlink("/tmp/test_ephemeral_agents.db-shm");
}

/* T186: ephemeral agent creation */
static int test_create_ephemeral(void) {
    cleanup();
    mkdir(AGENTS_DIR, 0755);

    sqlite3 *db = db_open(DB_PATH);
    if (!db) FAIL("db_open");

    char *name = agent_create_ephemeral(AGENTS_DIR, db);
    if (!name) { db_close(db); FAIL("agent_create_ephemeral returned NULL"); }

    /* Verify name format: ephemeral-<uuid> */
    if (strncmp(name, "ephemeral-", 10) != 0) { free(name); db_close(db); FAIL("bad name prefix"); }
    if (strlen(name) != 10 + 36) { free(name); db_close(db); FAIL("bad name length"); }

    /* Verify directory exists */
    char path[512];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", AGENTS_DIR, name);
    assert(stat(path, &st) == 0 && S_ISDIR(st.st_mode));

    /* Verify workspace subdir */
    snprintf(path, sizeof(path), "%s/%s/workspace", AGENTS_DIR, name);
    assert(stat(path, &st) == 0 && S_ISDIR(st.st_mode));

    /* T196: agent.json no longer written — config in daemon.db */

    /* Verify agent.db created */
    snprintf(path, sizeof(path), "%s/%s/agent.db", AGENTS_DIR, name);
    assert(stat(path, &st) == 0 && S_ISREG(st.st_mode));

    /* Verify seeded in daemon DB */
    AgentRow *row = db_agent_get(db, name);
    assert(row != NULL);
    assert(strcmp(row->name, name) == 0);
    agent_row_free(row);

    /* Two ephemeral agents get different names */
    char *name2 = agent_create_ephemeral(AGENTS_DIR, db);
    assert(name2 != NULL);
    assert(strcmp(name, name2) != 0);
    assert(strncmp(name2, "ephemeral-", 10) == 0);

    free(name2);
    free(name);
    db_close(db);
    cleanup();
    printf("PASS: test_create_ephemeral\n");
    return 0;
}

int main(void) {
    if (test_create_ephemeral()) return 1;
    printf("All ephemeral agent tests passed.\n");
    return 0;
}
