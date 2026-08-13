#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "context.h"
#include "test_util.h"

static void test_session_tmp_dir(void) {
    char buf[256];
    unsetenv("CCLAW_WORKSPACE");
    /* db-less fallback */
    session_tmp_dir(NULL, 42, buf, sizeof(buf));
    assert(strcmp(buf, "/tmp/cclaw-42") == 0);
    /* operator-pinned workspace wins */
    setenv("CCLAW_WORKSPACE", "/ws", 1);
    session_tmp_dir(NULL, 42, buf, sizeof(buf));
    assert(strcmp(buf, "/ws/.tool_results/42") == 0);
    unsetenv("CCLAW_WORKSPACE");
    printf("  PASS test_session_tmp_dir\n");
}

static void test_session_tmp_dir_agent(void) {
    /* Per-agent derivation from the session row: <db_dir>/agents/<agent>/
     * workspace/.tool_results/<sid> — the sandbox-visible spill home. */
    char dbp[] = "/tmp/cclaw_test_ctx_edge.db";
    unlink(dbp);
    sqlite3 *db = NULL;
    assert(sqlite3_open(dbp, &db) == SQLITE_OK);
    assert(sqlite3_exec(db,
        "CREATE TABLE sessions (id INTEGER PRIMARY KEY, agent_name TEXT);"
        "INSERT INTO sessions VALUES (7, 'alice'), (8, 'bad/name');",
        NULL, NULL, NULL) == SQLITE_OK);
    char buf[512];
    session_tmp_dir(db, 7, buf, sizeof(buf));
    assert(strcmp(buf, "/tmp/agents/alice/workspace/.tool_results/7") == 0);
    /* path-hostile agent name falls back to /tmp */
    session_tmp_dir(db, 8, buf, sizeof(buf));
    assert(strcmp(buf, "/tmp/cclaw-8") == 0);
    /* unknown session falls back too */
    session_tmp_dir(db, 99, buf, sizeof(buf));
    assert(strcmp(buf, "/tmp/cclaw-99") == 0);
    sqlite3_close(db);
    unlink(dbp);
    printf("  PASS test_session_tmp_dir_agent\n");
}

int main(void) {
    TEST_INIT();
    alarm(10);
    printf("test_context_edge:\n");
    test_session_tmp_dir();
    test_session_tmp_dir_agent();
    printf("All context_edge tests passed.\n");
    return 0;
}
