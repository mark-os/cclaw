#include "janitor.h"
#include <stdio.h>

/* V16,V19: Atomic recovery of stale locks and orphan pending sessions.
 * Increments error_count on crash detection; quarantines sessions at >= 3. */
int janitor_sweep(sqlite3 *db, int stale_timeout_sec) {
    if (!db || stale_timeout_sec <= 0) return -1;

    int recovered = 0;
    sqlite3_stmt *stmt;

    /* Phase 1: Stale locks → error state, increment error_count */
    const char *stale_sql =
        "UPDATE sessions SET state='error', lock_holder=NULL, lock_acquired_at=NULL,"
        " error_count=error_count+1, updated_at=unixepoch()"
        " WHERE state='running' AND lock_acquired_at IS NOT NULL"
        " AND (unixepoch() - lock_acquired_at) > ?;";
    if (sqlite3_prepare_v2(db, stale_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, stale_timeout_sec);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    /* Phase 2: Quarantine sessions with error_count >= 3 */
    const char *quarantine_sql =
        "UPDATE sessions SET state='quarantine', updated_at=unixepoch()"
        " WHERE state='error' AND error_count >= 3;";
    if (sqlite3_prepare_v2(db, quarantine_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    /* Phase 3: Recover error sessions (error_count < 3) → idle */
    const char *recover_sql =
        "UPDATE sessions SET state='idle', updated_at=unixepoch()"
        " WHERE state='error' AND error_count < 3;";
    if (sqlite3_prepare_v2(db, recover_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_step(stmt);
        recovered += sqlite3_changes(db);
        sqlite3_finalize(stmt);
    }

    /* Phase 4: Orphan pending → idle */
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
