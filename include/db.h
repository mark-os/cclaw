#ifndef CCLAW_DB_H
#define CCLAW_DB_H

#include "sqlite3.h"
#include "types.h"
#include <sys/types.h>

/* T88: Spawn queue request */
typedef struct {
    int64_t id;
    int64_t parent_session_id;
    char *task;
    int background;
    int depth;
    char *tool_call_id;
} SpawnRequest;

/* Open DB at path, set WAL mode + pragmas, create tables.
 * Returns NULL on failure. */
sqlite3 *db_open(const char *path);

/* Close DB handle. */
void db_close(sqlite3 *db);

/* Session CRUD (V14) */

/* Create session, returns id (>0) or -1 on error. agent_name may be NULL.
 * parent_session_id = -1 for top-level sessions. depth = 0 for top-level. */
int64_t session_create(sqlite3 *db, const char *name, const char *agent_name,
                       int64_t parent_session_id, int depth);

/* List all sessions. Caller must free returned array and each session's name.
 * Sets *count. Returns NULL on error or empty. */
Session *session_list(sqlite3 *db, int *count);

/* Get branch from leaf→root for session. Returns entries in root→leaf order.
 * Caller must free returned array and entry string fields. Sets *count. */
Entry *session_get_branch(sqlite3 *db, int64_t session_id, int *count);

/* V20: Get agent_name for session. Returns heap-allocated string or NULL. */
char *session_get_agent_name(sqlite3 *db, int64_t session_id);

/* Get depth for session. Returns depth (0 = top-level) or 0 on error. */
int session_get_depth(sqlite3 *db, int64_t session_id);

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

/* V3: Sub-agent limits — count active child sessions */
int session_count_children(sqlite3 *db, int64_t parent_session_id);
int session_count_active_agents(sqlite3 *db);

/* V17: Get next turn_id for a session (MAX(turn_id)+1, or 1 if none). */
int64_t db_next_turn_id(sqlite3 *db, int64_t session_id);

/* V17: Append entry with explicit turn_id. Like entry_append but tags the entry. */
int64_t entry_append_with_turn(sqlite3 *db, int64_t session_id, const Message *msg, int64_t turn_id);

/* Set session state (idle, running, waiting, error). Returns 0 on success. */
int session_set_state(sqlite3 *db, int64_t session_id, const char *state);

/* V18: Inbox primitives */
typedef struct {
    int64_t id;
    int64_t session_id;
    char *source;
    char *payload;
    int64_t created_at;
} InboxItem;

/* Insert a message into the inbox. Returns row id (>0) or -1 on error. */
int64_t inbox_insert(sqlite3 *db, int64_t session_id, const char *source, const char *payload);

/* Peek at unconsumed inbox items for a session (oldest first, max `limit`).
 * Caller must free with inbox_items_free. Sets *count. Returns NULL on error/empty. */
InboxItem *inbox_peek(sqlite3 *db, int64_t session_id, int limit, int *count);

/* Free an InboxItem array returned by inbox_peek. */
void inbox_items_free(InboxItem *items, int count);

/* Count unconsumed inbox items for a session. Returns count or -1 on error. */
int inbox_count(sqlite3 *db, int64_t session_id);

/* V18: Atomically consume unconsumed inbox items into session entries.
 * Within BEGIN EXCLUSIVE: marks items consumed, inserts as user messages, updates leaf.
 * Returns number of items consumed (≥0) or -1 on error (transaction rolled back). */
int inbox_consume_into_entries(sqlite3 *db, int64_t session_id, int limit);

/* T88: Spawn queue — agent processes post requests, daemon picks up + forks */
int64_t spawn_queue_insert(sqlite3 *db, int64_t parent_session_id, const char *task,
                           int background, int depth, const char *tool_call_id);
SpawnRequest *spawn_queue_peek_pending(sqlite3 *db, int *count);
int spawn_queue_mark(sqlite3 *db, int64_t id, const char *status, int64_t child_session_id);
void spawn_request_free(SpawnRequest *list, int count);

#endif
