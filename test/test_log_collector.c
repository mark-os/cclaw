/* Test: T218/V75 — log collector process.
 * Verifies: fd passing via SCM_RIGHTS, line buffering, batch insert to journal.db. */
#define _DEFAULT_SOURCE
#include "log_collector.h"
#include "db.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

static const char *JOURNAL_PATH = "/tmp/test_log_collector_journal.db";

int main(void) {
    alarm(10);
    unlink(JOURNAL_PATH);

    /* Start collector */
    int rc = log_collector_start(JOURNAL_PATH);
    assert(rc == 0);
    assert(log_collector_pid() > 0);

    /* Create a pipe, write some lines, send read end to collector */
    int pfd[2];
    rc = pipe(pfd);
    assert(rc == 0);

    rc = log_collector_send_fd(pfd[0], "test-agent", 1234, 42, 1);
    assert(rc == 0);
    close(pfd[0]); /* daemon closes after sending */

    /* Write lines to the pipe */
    const char *line1 = "hello from agent\n";
    const char *line2 = "second line\n";
    write(pfd[1], line1, strlen(line1));
    write(pfd[1], line2, strlen(line2));
    close(pfd[1]); /* EOF triggers flush */

    /* Also test stderr stream */
    int pfd2[2];
    rc = pipe(pfd2);
    assert(rc == 0);
    rc = log_collector_send_fd(pfd2[0], "test-agent", 1234, 42, 2);
    assert(rc == 0);
    close(pfd2[0]);
    const char *err_line = "error output\n";
    write(pfd2[1], err_line, strlen(err_line));
    close(pfd2[1]);

    /* Give collector time to process (100ms timer + epoll) */
    usleep(400000);

    /* Stop collector (flushes remaining) */
    log_collector_stop();

    /* Verify journal.db contents */
    sqlite3 *db = db_open_journal(JOURNAL_PATH);
    assert(db != NULL);

    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(db,
        "SELECT source, pid, session_id, stream, line FROM log ORDER BY id",
        -1, &stmt, NULL);
    assert(rc == SQLITE_OK);

    /* Row 1: stdout line 1 */
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "test-agent") == 0);
    assert(sqlite3_column_int(stmt, 1) == 1234);
    assert(sqlite3_column_int64(stmt, 2) == 42);
    assert(sqlite3_column_int(stmt, 3) == 1);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 4), "hello from agent") == 0);

    /* Row 2: stdout line 2 */
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 4), "second line") == 0);
    assert(sqlite3_column_int(stmt, 3) == 1);

    /* Row 3: stderr line */
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 4), "error output") == 0);
    assert(sqlite3_column_int(stmt, 3) == 2);

    assert(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    unlink(JOURNAL_PATH);
    printf("PASS: test_log_collector\n");
    return 0;
}
