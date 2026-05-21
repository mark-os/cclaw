#ifndef CCLAW_DB_H
#define CCLAW_DB_H

#include "sqlite3.h"
#include "types.h"

/* Open DB at path, set WAL mode + pragmas, create tables.
 * Returns NULL on failure. */
sqlite3 *db_open(const char *path);

/* Close DB handle. */
void db_close(sqlite3 *db);

/* Session CRUD (V14) */

/* Create session, returns id (>0) or -1 on error. */
int64_t session_create(sqlite3 *db, const char *name);

/* List all sessions. Caller must free returned array and each session's name.
 * Sets *count. Returns NULL on error or empty. */
Session *session_list(sqlite3 *db, int *count);

/* Get branch from leaf→root for session. Returns entries in root→leaf order.
 * Caller must free returned array and entry string fields. Sets *count. */
Entry *session_get_branch(sqlite3 *db, int64_t session_id, int *count);

/* Set leaf_id for session. Returns 0 on success, -1 on error. */
int session_set_leaf(sqlite3 *db, int64_t session_id, int64_t leaf_id);

/* Free a Session array returned by session_list. */
void session_list_free(Session *sessions, int count);

/* Free an Entry array returned by session_get_branch. */
void entry_branch_free(Entry *entries, int count);

#endif
