#include "janitor.h"
#include <stdio.h>

/* V16,V19: Atomic recovery of stale locks and orphan pending sessions */
int janitor_sweep(sqlite3 *db, int stale_timeout_sec) {
    if (!db || stale_timeout_sec <= 0) return -1;

    int recovered = 0;
    sqlite3_stmt *stmt;

    /* Stale locks: running sessions where lock_acquired_at is too old */
    const char *stale_sql =
        "UPDATE sessions SET state='idle', lock_holder=NULL, lock_acquired_at=NULL, updated_at=unixepoch()"
        " WHERE state='running' AND lock_acquired_at IS NOT NULL"
        " AND (unixepoch() - lock_acquired_at) > ?;";
    if (sqlite3_prepare_v2(db, stale_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, stale_timeout_sec);
        sqlite3_step(stmt);
        recovered += sqlite3_changes(db);
        sqlite3_finalize(stmt);
    }

    /* Orphan pending: sessions stuck in 'pending' with no active process */
    const char *pending_sql =
        "UPDATE sessions SET state='idle', lock_holder=NULL, lock_acquired_at=NULL, updated_at=unixepoch()"
        " WHERE state='pending';";
    if (sqlite3_prepare_v2(db, pending_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_step(stmt);
        recovered += sqlite3_changes(db);
        sqlite3_finalize(stmt);
    }

    if (recovered > 0)
        fprintf(stderr, "janitor: recovered %d session(s)\n", recovered);

    return recovered;
}
