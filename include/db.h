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
 * Returns NULL on failure. (Legacy: creates all tables — agent schema + extras) */
sqlite3 *db_open(const char *path);

/* T195/V73: 3-DB openers — each creates only its own schema */
sqlite3 *db_open_cclaw(const char *path);
sqlite3 *db_open_agent(const char *path);
sqlite3 *db_open_journal(const char *path);

/* V57: Set mmap + reduced cache pragmas for agent processes. */
void db_set_agent_pragmas(sqlite3 *db);

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

/* Resolve deliverable response text from session branch.
 * Returns last non-empty assistant content, or NULL if none.
 * Caller frees returned string. */
char *get_response_text(sqlite3 *db, int64_t session_id);

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

/* V58,V59: Insert compaction summary and reparent.
 * Inserts ROLE_COMPACTION entry as child of last_kept_id.
 * Reparents first_after_id to point to the new summary node.
 * Returns compaction entry id (>0) or -1 on error. Does NOT update session leaf_id. */
int64_t entry_compact(sqlite3 *db, int64_t session_id, int64_t last_kept_id,
                      int64_t first_after_id, const char *summary);

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

/* V61,T170: Secret-aware kv access. kv_get_secret decrypts enc: prefix values.
 * kv_set_secret encrypts before storing. */
char *db_kv_get_secret(sqlite3 *db, const char *key);
int db_kv_set_secret(sqlite3 *db, const char *key, const char *value);

/* V52,T171: Set the 32-byte encryption key for secret kv operations.
 * Must be called before db_kv_get_secret/db_kv_set_secret. */
void db_set_secret_key(const uint8_t key[32]);

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
int inbox_clear_source(sqlite3 *db, int64_t session_id, const char *source);

/* V18: Atomically consume unconsumed inbox items into session entries.
 * Within BEGIN EXCLUSIVE: marks items consumed, inserts as user messages, updates leaf.
 * Returns number of items consumed (≥0) or -1 on error (transaction rolled back). */
int inbox_consume_into_entries(sqlite3 *db, int64_t session_id, int limit);

/* T88/T202: Spawn queue — cclaw.db only (V73). peek/mark used by daemon. */
SpawnRequest *spawn_queue_peek_pending(sqlite3 *db, int *count);
int spawn_queue_mark(sqlite3 *db, int64_t id, const char *status, int64_t child_session_id);
void spawn_request_free(SpawnRequest *list, int count);

/* T119: agents table — DB-authoritative agent identity */
typedef struct {
    int64_t id;
    char *name;
    char *config;         /* JSON string */
    char *system_prompt;
    char *heartbeat;
    int64_t created_at;
    int64_t updated_at;
} AgentRow;

/* Get agent row by name. Returns NULL if not found. Caller frees with agent_row_free. */
AgentRow *db_agent_get(sqlite3 *db, const char *name);

/* Insert or update agent row. Returns 0 on success, -1 on error. */
int db_agent_upsert(sqlite3 *db, const char *name, const char *config,
                    const char *system_prompt, const char *heartbeat);

/* Seed agent from disk if not in DB. Loads agent.json + system.md from agents_dir.
 * Returns agent row (from DB after potential seed). Caller frees with agent_row_free. */
AgentRow *db_agent_seed(sqlite3 *db, const char *agents_dir, const char *name);

/* Free AgentRow. */
void agent_row_free(AgentRow *row);

/* T146: approvals table — admin approval flow (V54) */
typedef struct {
    int64_t id;
    int64_t session_id;
    char *agent_name;
    char *tool_call_id;
    char *type;       /* whitelist_host|create_agent|model_change|tool_enable */
    char *payload;    /* JSON */
    char *status;     /* pending|approved|denied */
    int64_t admin_chat_id;
    int64_t created_at;
    int64_t resolved_at; /* 0 if unresolved */
} Approval;

/* Insert approval request. Returns row id (>0) or -1 on error. */
int64_t approval_insert(sqlite3 *db, int64_t session_id, const char *agent_name,
                        const char *tool_call_id, const char *type, const char *payload);

/* Get pending approvals. Caller frees with approval_list_free. Sets *count. */
Approval *approval_list_pending(sqlite3 *db, int *count);

/* Get approval by id. Returns NULL if not found. Caller frees with approval_free. */
Approval *approval_get(sqlite3 *db, int64_t id);

/* Resolve approval (approve or deny). Sets status + resolved_at + admin_chat_id. Returns 0 on success. */
int approval_resolve(sqlite3 *db, int64_t id, const char *status, int64_t admin_chat_id);

/* T148: Get pending approvals not yet notified to admins. Caller frees with approval_list_free. */
Approval *approval_list_unnotified(sqlite3 *db, int *count);

/* T148: Mark approval as notified (sent to admins). Returns 0 on success. */
int approval_mark_notified(sqlite3 *db, int64_t id);

/* Free single Approval. */
void approval_free(Approval *a);

/* Free Approval array. */
void approval_list_free(Approval *list, int count);

/* T152: memory_blocks table (V55) */
typedef struct {
    int64_t id;
    char *agent_name;
    char *label;
    char *value;
    char *description;
    int char_limit;
    int read_only;
    int64_t created_at;
    int64_t updated_at;
} MemoryBlock;

/* Create a memory block. Returns row id (>0) or -1 on error.
 * Fails if UNIQUE(agent_name, label) violated. */
int64_t memory_block_create(sqlite3 *db, const char *agent_name, const char *label,
                            const char *description, const char *value, int char_limit);

/* Get a memory block by agent_name + label. Returns NULL if not found. Caller frees. */
MemoryBlock *memory_block_get(sqlite3 *db, const char *agent_name, const char *label);

/* List all memory blocks for an agent. Caller frees with memory_block_list_free. Sets *count. */
MemoryBlock *memory_block_list(sqlite3 *db, const char *agent_name, int *count);

/* Update block value. Enforces char_limit (V55). Returns 0 on success, -1 on error. */
int memory_block_set_value(sqlite3 *db, const char *agent_name, const char *label, const char *value);

/* Free a single MemoryBlock. */
void memory_block_free(MemoryBlock *mb);

/* Free a MemoryBlock array. */
void memory_block_list_free(MemoryBlock *list, int count);

/* Seed memory blocks from agent.json memory_blocks[] array (cJSON).
 * Only inserts blocks that don't already exist (DB authoritative). */
void memory_blocks_seed(sqlite3 *db, const char *agent_name, const char *agent_json_str);

/* T193/V69: Channel→agent binding.
 * db_channel_binding_get returns heap-allocated agent_name or NULL if unbound.
 * db_channel_binding_set upserts binding. Returns 0 on success, -1 on error. */
char *db_channel_binding_get(sqlite3 *db, const char *channel_type, const char *channel_id);
int db_channel_binding_set(sqlite3 *db, const char *channel_type, const char *channel_id, const char *agent_name);

/* Pending entry helpers (used by daemon and CLI for exit code dispatch) */
char *find_pending_entry(sqlite3 *adb, int64_t session_id, int64_t *entry_id);
char *read_tool_call_args(sqlite3 *adb, int64_t session_id,
                          const char *tool_call_id, char **tool_name);
int update_pending_entry(sqlite3 *adb, int64_t entry_id, const char *result);

/* T268: Sum cost_nano for all entries in a session. Returns total nanodollars. */
int64_t session_cost(sqlite3 *db, int64_t session_id);

#endif
