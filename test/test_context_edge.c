#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "context.h"
#include "run_tool.h"
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
}

static void test_session_tmp_dir_agent(void) {
    /* Per-agent derivation from the session row: <db_dir>/agents/<agent>/
     * workspace/.tool_results/<sid> — the sandbox-visible spill home. */
    char dbp[] = "/tmp/cclaw_test_ctx_edge.db";
    unlink(dbp);
    sqlite3 *db = NULL;
    assert((db = db_open(dbp)) != NULL);
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
}

/* A byte-budget cut that lands inside a multi-byte character must back up to
 * the character's first byte: half a character is invalid UTF-8, and the
 * provider rejects the whole request body that carries it. */
static void test_spill_cut_keeps_utf8_whole(void) {
    sqlite3 *db = test_db_open(":memory:");
    size_t n = RUNTOOL_RESULT_MAX + 64;
    char *src = malloc(n + 1);
    memset(src, 'a', n);
    src[n] = '\0';
    /* U+00E9 (C3 A9) straddling the budget: lead byte kept, tail cut. */
    src[RUNTOOL_RESULT_MAX - 1] = (char)0xC3;
    src[RUNTOOL_RESULT_MAX]     = (char)0xA9;

    char *out = truncate_and_spill(db, src, 7, "call_utf8");
    char *note = out ? strstr(out, "\n[truncated") : NULL;
    assert(note);                                   /* it did truncate */
    assert(note > out && note[-1] == 'a');          /* no orphaned lead byte */
    assert((size_t)(note - out) == RUNTOOL_RESULT_MAX - 1);

    /* Control: a cut on a character boundary keeps the full budget. */
    src[RUNTOOL_RESULT_MAX - 1] = 'a';
    src[RUNTOOL_RESULT_MAX]     = 'a';
    char *out2 = truncate_and_spill(db, src, 7, "call_utf8");
    char *note2 = out2 ? strstr(out2, "\n[truncated") : NULL;
    assert(note2 && (size_t)(note2 - out2) == RUNTOOL_RESULT_MAX);

    char path[512];
    assert(spill_path_build(db, 7, "call_utf8", path, sizeof(path)) == 0);
    unlink(path);
    free(out); free(out2); free(src);
    sqlite3_close(db);
}

int main(void) {
    TEST_INIT();
    alarm(10);
    printf("test_context_edge:\n");
    RUN(test_session_tmp_dir);
    RUN(test_session_tmp_dir_agent);
    RUN(test_spill_cut_keeps_utf8_whole);
    printf("All context_edge tests passed.\n");
    return 0;
}
