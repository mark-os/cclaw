#ifndef CCLAW_CHANGE_LOG_H
#define CCLAW_CHANGE_LOG_H

/* The change log (specs/operations.md "Change log"): triggers on the
 * configuration tables write one change_log row per row changed, naming the
 * handle that did it (cclaw_actor()) and what it was doing (cclaw_context()).
 *
 * CHANGE_LOG_COLUMNS is the one list that says which tables are logged and,
 * for every column of each, whether it is a setting or a measurement. The
 * triggers are generated from it (change_log_sync), and the data explorer
 * groups fields by it — nothing else may restate the classification. Adding a
 * table to the log is adding its columns here. */

#include "sqlite3.h"
#include <stddef.h>

typedef enum {
    COL_SETTING,      /* configuration: a change is logged, old/new values shown */
    COL_MEASUREMENT,  /* observed state (counters, timestamps, leases): never logged */
    COL_SECRET        /* a setting whose content must never be logged: the fact
                         of a change is recorded, the value is not */
} ColumnClass;

typedef struct {
    const char *table;
    const char *column;
    ColumnClass cls;
} ChangeLogColumn;

extern const ChangeLogColumn CHANGE_LOG_COLUMNS[];
extern const size_t CHANGE_LOG_COLUMN_COUNT;

/* Class of table.column; -1 when the column is not in the list. */
int change_log_class(const char *table, const char *column);

/* Make the change_log_* triggers match the list: create missing ones, replace
 * any whose text differs, drop ones for tables no longer listed. Writes only
 * when something differs (in its own BEGIN IMMEDIATE unless a transaction is
 * already open on this handle). 0 on success. */
int change_log_sync(sqlite3 *db);

/* The generated CREATE TRIGGER statement for one table and action
 * ("ins"/"upd"/"del"), as change_log_sync would install it. Heap; NULL for an
 * unlisted table. For tests that compare a database against the list. */
char *change_log_trigger_sql(sqlite3 *db, const char *table, const char *action);

#endif
