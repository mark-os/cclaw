#ifndef CCLAW_DB_H
#define CCLAW_DB_H

#include "sqlite3.h"
#include "types.h"
#include <sys/types.h>

/* Sub-agent info struct */
typedef struct {
    int64_t id;
    int64_t parent_session_id;
    int64_t session_id;
    pid_t pid;
    int depth;
    char *status;
    char *task;
    char *result;
} SubAgentInfo;

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

/* V14: Append entry as child of current leaf, update session leaf_id.
 * If session has no entries (leaf_id == -1), entry becomes root (parent_id = -1).
 * Returns new entry id (>0) or -1 on error. */
int64_t entry_append(sqlite3 *db, int64_t session_id, const Message *msg);

/* V14: Append entry as child of a specific parent (for branching).
 * Updates session leaf_id to new entry. Returns new entry id or -1. */
int64_t entry_append_at(sqlite3 *db, int64_t session_id, int64_t parent_id, const Message *msg);

/* Free a Session array returned by session_list. */
void session_list_free(Session *sessions, int count);

/* Free an Entry array returned by session_get_branch. */
void entry_branch_free(Entry *entries, int count);

/* V7: FTS5 full-text search over message content within a session.
 * Returns matching entries ranked by relevance (max 50). Caller frees with entry_branch_free. */
Entry *entry_search(sqlite3 *db, const char *query, int64_t session_id, int *count);

/* Key-value store. db_kv_get returns malloc'd string or NULL. Caller frees. */
char *db_kv_get(sqlite3 *db, const char *key);
int db_kv_set(sqlite3 *db, const char *key, const char *value);

/* T26: Telegram chat_id → session routing.
 * db_tg_get_session returns session_id or -1 if not mapped.
 * db_tg_set_session returns 0 on success, -1 on error. */
int64_t db_tg_get_session(sqlite3 *db, int64_t chat_id);
int db_tg_set_session(sqlite3 *db, int64_t chat_id, int64_t session_id);

/* V3: Sub-agent tracking */
int64_t subagent_create(sqlite3 *db, int64_t parent_session_id, int64_t session_id,
                        pid_t pid, int depth, const char *task);
int subagent_count_by_parent(sqlite3 *db, int64_t parent_session_id);
int subagent_count_total(sqlite3 *db);
int subagent_finish(sqlite3 *db, int64_t agent_id, const char *status, const char *result);
SubAgentInfo *subagent_get(sqlite3 *db, int64_t agent_id);
void subagent_info_free(SubAgentInfo *info);

/* List all running sub-agents. Caller must free array and each element's fields via subagent_info_free logic. */
SubAgentInfo *subagent_list_running(sqlite3 *db, int *count);

#endif
