/* T67: Test CLI acquire/release/refresh keep-alive pattern */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "db.h"

int main(void) {
    sqlite3 *db = db_open(":memory:");
    assert(db);

    int64_t sid = session_create(db, "cli-test", NULL);
    assert(sid > 0);

    /* Acquire succeeds */
    assert(session_try_acquire(db, sid, "cli-123") == 0);

    /* Refresh succeeds while held */
    assert(session_refresh_lock(db, sid, "cli-123") == 0);

    /* Refresh fails for wrong holder */
    assert(session_refresh_lock(db, sid, "other") == -1);

    /* Second acquire fails (already running) */
    assert(session_try_acquire(db, sid, "cli-456") == -1);

    /* Release succeeds */
    assert(session_release(db, sid, "cli-123") == 0);

    /* Refresh fails after release (not running) */
    assert(session_refresh_lock(db, sid, "cli-123") == -1);

    /* Can re-acquire after release */
    assert(session_try_acquire(db, sid, "cli-456") == 0);
    assert(session_release(db, sid, "cli-456") == 0);

    db_close(db);
    printf("test_cli_lock: all passed\n");
    return 0;
}
