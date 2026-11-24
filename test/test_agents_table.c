#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <sys/stat.h>

#define AGENTS_DIR "/tmp/test_agents_t119"

/* An agent row seeds from its disk files on first reference; after that the
 * DB is authoritative and the disk copy is ignored. */
static void test_seed_then_db_authoritative(void) {
    mkdir(AGENTS_DIR, 0755);
    mkdir(AGENTS_DIR "/Mybot", 0755);
    FILE *f = fopen(AGENTS_DIR "/Mybot/system.md", "w");
    assert(f);
    fprintf(f, "You are Mybot.");
    fclose(f);

    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    assert(db_agent_get(db, "Mybot") == NULL);

    AgentRow *row = db_agent_seed(db, AGENTS_DIR, "Mybot");
    assert(row && strcmp(row->name, "Mybot") == 0);
    assert(strcmp(row->system_prompt, "You are Mybot.") == 0);
    agent_row_free(row);

    assert(db_agent_upsert(db, "Mybot", NULL, "DB prompt.") == 0);
    row = db_agent_seed(db, AGENTS_DIR, "Mybot");
    assert(row && strcmp(row->system_prompt, "DB prompt.") == 0);
    agent_row_free(row);

    /* No agents dir still creates an empty row. */
    row = db_agent_seed(db, "/nonexistent/path", "Ghost");
    assert(row && row->system_prompt == NULL);
    agent_row_free(row);
    row = db_agent_get(db, "Ghost");
    assert(row);
    agent_row_free(row);

    db_close(db);
    unlink(AGENTS_DIR "/Mybot/system.md");
    rmdir(AGENTS_DIR "/Mybot");
    rmdir(AGENTS_DIR);
}

int main(void) {
    TEST_INIT();
    RUN(test_seed_then_db_authoritative);
    return 0;
}
