/* Inbox cap: an inbox item over RUNTOOL_RESULT_MAX is cut when it becomes a
 * user entry, the full text spilled to the agent's workspace, and the entry
 * names the file. A failed spill still cuts and says the text was not saved. */
#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "run_tool.h"
#include "test_util.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *DB_PATH = "/tmp/test_cclaw_inbox_cap.sqlite";
static const char *WS_DIR = "/tmp/test_cclaw_inbox_cap_ws";
static const char *WS_BLOCKER = "/tmp/test_cclaw_inbox_cap_blocker";

static void clean_all(void) {
    test_db_clean(DB_PATH);
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s", WS_DIR, WS_BLOCKER);
    if (system(cmd) != 0) { /* best effort */ }
}

static char *filled(size_t n, char c) {
    char *s = malloc(n + 1);
    assert(s);
    memset(s, c, n);
    s[n] = '\0';
    return s;
}

/* Drain the session's inbox and return the one resulting entry's content
 * and content_bytes. */
static char *drain_one(sqlite3 *db, int64_t sid, int *content_bytes) {
    assert(inbox_consume_into_entries(db, sid, 10) == 1);
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "SELECT content, content_bytes FROM entries WHERE session_id=?1"
        " ORDER BY id DESC LIMIT 1", -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    char *c = test_strdup_((const char *)sqlite3_column_text(s, 0));
    if (content_bytes) *content_bytes = sqlite3_column_int(s, 1);
    sqlite3_finalize(s);
    return c;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    assert(buf);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    *len = got;
    return buf;
}

static void test_under_cap_untouched(void) {
    setenv("CCLAW_WORKSPACE", WS_DIR, 1);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "cap_under", NULL, -1, 0);
    assert(sid > 0);

    char *msg = filled(RUNTOOL_RESULT_MAX, 'x');   /* exactly at the cap */
    int64_t iid = inbox_insert(db, sid, "cli", NULL, msg);
    assert(iid > 0);
    int cb = 0;
    char *got = drain_one(db, sid, &cb);
    assert(strcmp(got, msg) == 0);
    assert(cb == RUNTOOL_RESULT_MAX);

    char path[512];
    snprintf(path, sizeof(path), "%s/.tool_results/%lld/inbox-%lld.out",
             WS_DIR, (long long)sid, (long long)iid);
    assert(access(path, F_OK) != 0);

    free(got); free(msg);
    db_close(db);
    printf("  PASS test_under_cap_untouched\n");
}

static void test_over_cap_spilled(void) {
    setenv("CCLAW_WORKSPACE", WS_DIR, 1);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "cap_over", NULL, -1, 0);
    assert(sid > 0);

    size_t n = RUNTOOL_RESULT_MAX * 2 + 17;
    char *msg = filled(n, 'y');
    msg[n - 1] = 'Z';   /* the tail must survive in the file, not the entry */
    int64_t iid = inbox_insert(db, sid, "cli", NULL, msg);
    assert(iid > 0);
    int cb = 0;
    char *got = drain_one(db, sid, &cb);

    char path[512];
    snprintf(path, sizeof(path), "%s/.tool_results/%lld/inbox-%lld.out",
             WS_DIR, (long long)sid, (long long)iid);

    /* Head kept, note appended, entry sized by what it holds. */
    assert(strncmp(got, msg, RUNTOOL_RESULT_MAX) == 0);
    assert(got[RUNTOOL_RESULT_MAX] == '\n');
    assert(strstr(got, "[truncated") != NULL);
    assert(strstr(got, path) != NULL);
    assert(strchr(got, 'Z') == NULL);
    assert((size_t)cb == strlen(got));

    size_t flen = 0;
    char *file = read_file(path, &flen);
    assert(file);
    assert(flen == n);
    assert(memcmp(file, msg, n) == 0);

    free(file); free(got); free(msg);
    db_close(db);
    printf("  PASS test_over_cap_spilled\n");
}

static void test_multibyte_boundary(void) {
    setenv("CCLAW_WORKSPACE", WS_DIR, 1);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "cap_utf8", NULL, -1, 0);
    assert(sid > 0);

    /* A 3-byte character (U+20AC, E2 82 AC) straddles the cap: its lead byte
     * at MAX-2, continuations at MAX-1 and MAX. The cut must back up to the
     * lead byte, keeping MAX-2 bytes. */
    size_t n = RUNTOOL_RESULT_MAX + 100;
    char *msg = filled(n, 'a');
    msg[RUNTOOL_RESULT_MAX - 2] = (char)0xE2;
    msg[RUNTOOL_RESULT_MAX - 1] = (char)0x82;
    msg[RUNTOOL_RESULT_MAX]     = (char)0xAC;
    assert(inbox_insert(db, sid, "cli", NULL, msg) > 0);
    char *got = drain_one(db, sid, NULL);

    assert(strncmp(got, msg, RUNTOOL_RESULT_MAX - 2) == 0);
    assert(got[RUNTOOL_RESULT_MAX - 2] == '\n');
    assert(strstr(got, "[truncated") != NULL);

    free(got); free(msg);
    db_close(db);
    printf("  PASS test_multibyte_boundary\n");
}

static void test_spill_failure_degrades(void) {
    /* A regular file where the workspace directory should be: mkdir and open
     * both fail, so the spill cannot be written. */
    int fd = open(WS_BLOCKER, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    close(fd);
    char ws[256];
    snprintf(ws, sizeof(ws), "%s/ws", WS_BLOCKER);
    setenv("CCLAW_WORKSPACE", ws, 1);

    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "cap_fail", NULL, -1, 0);
    assert(sid > 0);

    size_t n = RUNTOOL_RESULT_MAX + 4096;
    char *msg = filled(n, 'q');
    assert(inbox_insert(db, sid, "cli", NULL, msg) > 0);
    char *got = drain_one(db, sid, NULL);   /* consumed: the turn goes on */

    assert(strncmp(got, msg, RUNTOOL_RESULT_MAX) == 0);
    assert(got[RUNTOOL_RESULT_MAX] == '\n');
    assert(strstr(got, "could not be saved") != NULL);
    assert(strstr(got, "Full message:") == NULL);
    assert(inbox_count(db, sid) == 0);

    free(got); free(msg);
    db_close(db);
    unsetenv("CCLAW_WORKSPACE");
    printf("  PASS test_spill_failure_degrades\n");
}

int main(void) {
    TEST_INIT();
    clean_all();
    printf("test_inbox_cap:\n");
    test_under_cap_untouched();
    test_over_cap_spilled();
    test_multibyte_boundary();
    test_spill_failure_degrades();
    clean_all();
    printf("All inbox cap tests passed.\n");
    return 0;
}
