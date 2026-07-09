#ifndef CCLAW_DB_H
#define CCLAW_DB_H

#include "sqlite3.h"
#include "types.h"
#include "tool_shell.h"   /* ShellSecret */
#include <sys/types.h>

/* Install the process-global SQLite error log handler. Must be called once,
 * before the first db_open() (i.e. before sqlite3_initialize runs). */
void db_configure_logging(void);

/* Open DB at path, set WAL mode + pragmas, create tables.
 * Returns NULL on failure. */
sqlite3 *db_open(const char *path);

/* Apply schema (CREATE TABLE IF NOT EXISTS). Call once from main process. */
int db_ensure_schema(sqlite3 *db);

/* 1 if the DB's schema generation is compatible with this build (fresh/empty,
 * current, or successfully patched forward), 0 if it can't be used and must be
 * deleted. Check before db_ensure_schema — a stale DB silently misbehaves
 * otherwise. Applies pending schema patches as a side effect. */
int db_schema_compat(sqlite3 *db);

/* Read-only schema inspection — reports where a DB stands relative to this
 * build without applying patches (unlike db_schema_compat). For --doctor. */
typedef enum {
    DB_SCHEMA_FRESH,      /* empty — initialized at the current version on first run */
    DB_SCHEMA_CURRENT,    /* stamped with this build's version */
    DB_SCHEMA_UPGRADABLE, /* older, in patch range — auto-upgraded at next startup */
    DB_SCHEMA_FUTURE,     /* stamped by a newer build — refused */
    DB_SCHEMA_TOO_OLD     /* predates migration tracking — refused, delete it */
} DbSchemaState;
DbSchemaState db_schema_state(sqlite3 *db, int *user_version);

/* Seed default config + provider. Call once from main() on first run. */
int db_seed_defaults(sqlite3 *db);

/* Set mmap + reduced cache pragmas for child processes (short-lived). */
void db_set_child_pragmas(sqlite3 *db);

/* Enable sqlite3_trace_v2 profiling (slow queries → WARN, all → TRACE). */
void db_enable_trace(sqlite3 *db);

/* Slow-query WARN threshold in ms (config key sql_slow_ms, default 100).
 * 0 disables the warning; statements still show at TRACE. */
void db_set_slow_query_ms(int ms);

/* Close DB handle. */
void db_close(sqlite3 *db);

/* Session CRUD */

/* Create session, returns id (>0) or -1 on error. agent_name may be NULL.
 * parent_session_id = -1 for top-level sessions. depth = 0 for top-level. */
int64_t session_create(sqlite3 *db, const char *name, const char *agent_name,
                       int64_t parent_session_id, int depth);

/* As session_create, but freezes a per-session tool scope. tool_filter is a
 * JSON array of tool names (NULL = unrestricted); anything else fails the
 * insert. Effective tools = grants ∩ filter — the filter never grants. */
int64_t session_create_filtered(sqlite3 *db, const char *name, const char *agent_name,
                                int64_t parent_session_id, int depth,
                                const char *tool_filter);

/* 1 if session's tool_filter is NULL or contains tool_name; 0 otherwise
 * (including unknown session — fail closed). */
int session_tool_allowed(sqlite3 *db, int64_t session_id, const char *tool_name);

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

/* Get agent_name for session. Returns heap-allocated string or NULL. */
char *session_get_agent_name(sqlite3 *db, int64_t session_id);

/* Get depth for session. Returns depth (0 = top-level) or 0 on error. */
int session_get_depth(sqlite3 *db, int64_t session_id);

/* Set leaf_id for session. Returns 0 on success, -1 on error. */

/* Append entry as child of current leaf, update session leaf_id.
 * If session has no entries (leaf_id == -1), entry becomes root (parent_id = -1).
 * Returns new entry id (>0) or -1 on error. */

/* Append entry as child of a specific parent (for branching).
 * Updates session leaf_id to new entry. Returns new entry id or -1. */

/* Insert compaction summary and reparent.
 * Inserts ROLE_COMPACTION entry as child of last_kept_id.
 * Reparents first_after_id to point to the new summary node.
 * Returns compaction entry id (>0) or -1 on error. Does NOT update session leaf_id. */
int64_t entry_compact(sqlite3 *db, int64_t session_id, int64_t last_kept_id,
                      int64_t first_after_id, const char *summary);

/* Free a Session array returned by session_list. */
void session_list_free(Session *sessions, int count);

/* Free an Entry array returned by session_get_branch. */
void entry_branch_free(Entry *entries, int count);

/* FTS5 full-text search over message content within a session.
 * Returns matching entries ranked by relevance (max 50). Caller frees with entry_branch_free. */
Entry *entry_search(sqlite3 *db, const char *query, int64_t session_id, int *count);

/* Set the 32-byte encryption key for secrets-table operations.
 * Must be called before any db_secret_* read/write. */
void db_set_secret_key(const uint8_t key[32]);

/* Wipe the master key from this process's memory. Used in the forked tool-exec
 * broker so the relay process carries no key material. */
void db_wipe_secret_key(void);

/* Returns 1 if the master secret key is loaded in this process, 0 otherwise. */
int db_secret_key_loaded(void);

/* Sub-agent limits — count active child sessions */
int session_count_children(sqlite3 *db, int64_t parent_session_id);
int session_count_active_agents(sqlite3 *db);

/* Get next turn_id for a session (MAX(turn_id)+1, or 1 if none). */
int64_t db_next_turn_id(sqlite3 *db, int64_t session_id);

/* Append entry with explicit turn_id. Like entry_append but tags the entry. */
int64_t entry_append_with_turn(sqlite3 *db, int64_t session_id, const Message *msg, int64_t turn_id);

/* Append a flat typed entry. type is one of: "system", "user_message",
 * "assistant_message", "reasoning", "tool_call", "tool_result".
 * content: text payload. tool_call_id/tool_name used for tool_call/tool_result.
 * Returns new entry id or -1. */
int64_t entry_append_typed(sqlite3 *db, int64_t session_id, int64_t turn_id,
                           const char *type, int part_index, const char *content,
                           const char *tool_call_id, const char *tool_name,
                           int is_error, StopReason stop_reason,
                           const char *model, int usage_in, int usage_out,
                           int64_t cost_nano);

/* ── Process registry + per-session liveness ──────────────────────
 * A registry row is considered dead once its pid is gone or it has missed a
 * heartbeat for longer than PROCESS_TTL_SEC. The heartbeat fires from the
 * periodic loop well inside this window. */
#define PROCESS_TTL_SEC 60

/* This process's instance id (stamped into sessions.owner_instance by the
 * session_set_state CAS). Set once after process_register; "" until then. */
void db_set_instance_id(const char *id);
const char *db_get_instance_id(void);

/* Insert a registry row with a fresh random instance id (lower-hex of 16
 * random bytes). Writes the id into out_id. Returns 0 on success, -1 on error. */
int process_register(sqlite3 *db, const char *mode, int pid, char *out_id, size_t out_sz);

/* Refresh this process's heartbeat_at. Returns 0 on success. */
int process_heartbeat(sqlite3 *db, const char *id);

/* Remove this process's registry row (clean shutdown). Returns 0 on success. */
int process_unregister(sqlite3 *db, const char *id);

/* Prune dead registry rows (missed heartbeat > ttl_sec, or pid gone). Returns 0. */
int process_gc_dead(sqlite3 *db, int ttl_sec);

/* 1 if the given instance id has a fresh, pid-present registry row, else 0. */
int process_is_live(sqlite3 *db, const char *id, int ttl_sec);

/* Set session state (idle, running, waiting, error). Returns 0 on success. */
int session_set_state(sqlite3 *db, int64_t session_id, const char *state);
int session_set_leaf(sqlite3 *db, int64_t session_id, int64_t leaf_id);

/* Turn iteration tracking (advance_session uses this instead of in-memory counter) */
int session_get_iteration(sqlite3 *db, int64_t session_id);
int session_set_iteration(sqlite3 *db, int64_t session_id, int iter);
int session_bump_iteration(sqlite3 *db, int64_t session_id);

/* Parent info for sub-agent completion wake */
typedef struct {
    int64_t parent_session_id;
    char *parent_tool_call_id;  /* NULL if background mode */
} SessionParentInfo;
SessionParentInfo session_get_parent_info(sqlite3 *db, int64_t session_id);

/* Set parent_tool_call_id on a child session (blocking sub-agent) */
int session_set_parent_tool_call_id(sqlite3 *db, int64_t session_id, const char *call_id);

/* Inbox primitives */
typedef struct {
    int64_t id;
    int64_t session_id;
    char *source;
    char *payload;
    int64_t created_at;
} InboxItem;

/* Insert a message into the inbox. Returns row id (>0) or -1 on error. */
int64_t inbox_insert(sqlite3 *db, int64_t session_id, const char *source, const char *payload);

/* Scan payload for secrets, redact findings, then insert. */
int64_t inbox_insert_scanned(sqlite3 *db, int64_t session_id, const char *source, const char *payload);

/* Peek at unconsumed inbox items for a session (oldest first, max `limit`). */
InboxItem *inbox_peek(sqlite3 *db, int64_t session_id, int limit, int *count);

/* Free an InboxItem array returned by inbox_peek. */
void inbox_items_free(InboxItem *items, int count);

/* Count unconsumed inbox items for a session. Returns count or -1 on error. */
int inbox_count(sqlite3 *db, int64_t session_id);

/* Consume unconsumed inbox items into session entries (no transaction).
 * Caller must hold an open transaction. Returns count (≥0) or -1 on error. */
int inbox_consume_into_entries_locked(sqlite3 *db, int64_t session_id, int limit);

/* Atomically consume unconsumed inbox items into session entries.
 * Returns number of items consumed (≥0) or -1 on error (transaction rolled back). */
int inbox_consume_into_entries(sqlite3 *db, int64_t session_id, int limit);

/* agents table — DB-authoritative agent identity */
typedef struct {
    char *name;
    char *system_prompt;
    char *description;
    int64_t created_at;
} AgentRow;

/* Get agent row by name. Returns NULL if not found. Caller frees with agent_row_free. */
AgentRow *db_agent_get(sqlite3 *db, const char *name);

/* List all agent names from cclaw.db agents table.
 * Returns heap-allocated array of names (each heap-allocated). Sets *count.
 * Caller must free each name and the array. */
char **db_agent_list(sqlite3 *db, int *count);

/* Insert or update agent row. Returns 0 on success, -1 on error. */
int db_agent_upsert(sqlite3 *db, const char *name, const char *description,
                    const char *system_prompt);

/* Sensitivity axis (specs/trust.md): operator-owned host labels, global.
 * db_sensitive_hosts returns the kind='host' values as a heap array of heap
 * strings (NULL if none). Caller frees each entry and the array. add/rm
 * return 0 on success, -1 on error (rm: 0 even if absent). */
char **db_sensitive_hosts(sqlite3 *db, int *count);
int db_sensitive_host_add(sqlite3 *db, const char *host);
int db_sensitive_host_rm(sqlite3 *db, const char *host);

/* Secret-to-host bindings (specs/trust.md fail-closed credential rule).
 * db_secret_hosts returns the hosts bound to one secret name (heap array of
 * heap strings, NULL if none; caller frees). bind/unbind return 0/-1. */
char **db_secret_hosts(sqlite3 *db, const char *secret_name, int *count);
int db_secret_host_bind(sqlite3 *db, const char *secret_name, const char *host);
int db_secret_host_unbind(sqlite3 *db, const char *secret_name, const char *host);

/* Secret store (specs/security.md): DB-backed secrets, encrypted at rest with
 * the process-wide master key (db_set_secret_key). All fail -1 if the key
 * isn't loaded (db_secret_key_loaded() == 0).
 * db_secret_set: insert or replace name -> value (source/status/scope as
 * given; NULL scope = 'agent'). scope='system' rows are daemon-consumed
 * provider keys — excluded from db_secrets_load and thus from interpolation.
 * db_secret_rm: delete by name (0 even if absent).
 * db_secret_exists: 1 if a row with this name exists, 0 otherwise/on error.
 * db_secrets_load: decrypt every scope='agent' row into a heap ShellSecret
 * array (caller frees with shell_secrets_free); *count=0/NULL if none or key
 * not loaded.
 * db_secret_get_system: decrypted value of one scope='system' secret.
 * db_secret_pending_count: rows with status='pending' (quarantine spam guard).
 * db_secret_set_status: flip status (e.g. 'pending' -> 'active' on bind). */
int db_secret_set(sqlite3 *db, const char *name, const char *value,
                  const char *source, const char *status, const char *scope);
int db_secret_rm(sqlite3 *db, const char *name);
int db_secret_exists(sqlite3 *db, const char *name);
ShellSecret *db_secrets_load(sqlite3 *db, size_t *count);
char *db_secret_get_system(sqlite3 *db, const char *name);
int db_secret_pending_count(sqlite3 *db);
int db_secret_set_status(sqlite3 *db, const char *name, const char *status);

/* Seed agent from disk if not in DB. Loads agent.json + system.md from agents_dir.
 * Returns agent row (from DB after potential seed). Caller frees with agent_row_free. */
AgentRow *db_agent_seed(sqlite3 *db, const char *agents_dir, const char *name);

/* Free AgentRow. */
void agent_row_free(AgentRow *row);

/* Atomic agent rename: cascades to all tables. Returns:
 * 0=success, -1=busy (sessions active), -2=name conflict, -3=invalid name, -4=not found */
int agent_rename(sqlite3 *db, const char *old_name, const char *new_name,
                 int64_t requesting_session_id);

/* Tag an entry with the JSON array of network hosts its tool run contacted
 * (provenance for query-time untrusted-content wrapping). Returns 0 on ok. */
int db_entry_set_network_hosts(sqlite3 *db, int64_t entry_id, const char *hosts_json);

/* tool_calls status helpers */
typedef struct {
    char *call_id;
    char *name;
    char *arguments;
    int64_t entry_id;
    int64_t turn_id;
} PendingToolCall;

/* Set tool_call status. resolved_by may be NULL. Returns 0 on success. */
int db_tool_call_set_status(sqlite3 *db, int64_t session_id, const char *call_id,
                            const char *status, const char *resolved_by);

/* Mark a tool_call done and record the result entry, keyed by (session_id,
 * call_id). Like db_tool_call_complete_with_result but for callers that hold
 * the session_id rather than the assistant entry_id (sub-agent completion). */
int db_tool_call_complete_by_call(sqlite3 *db, int64_t session_id,
                                  const char *call_id, int64_t result_entry_id);

/* Get pending tool_calls for session. Caller frees with db_tool_call_free_pending. */
PendingToolCall *db_tool_call_get_pending(sqlite3 *db, int64_t session_id, int *out_count);

/* 1 if the session has any tool_call still in flight (status='running' — an
 * async tool dispatched but not yet completed: a forked tool child or a
 * sub-agent). The turn's tool phase isn't done until this returns 0. */
int db_tool_call_any_running(sqlite3 *db, int64_t session_id);

/* Free PendingToolCall array. */
void db_tool_call_free_pending(PendingToolCall *list, int count);

/* memory_blocks table */
typedef struct {
    int64_t id;
    char *agent_name;
    char *label;
    char *value;
    char *description;
    int char_limit;
    int read_only;
    char *placement;  /* 'system' (system prompt) or 'context' (per-turn session context) */
    int64_t created_at;
    int64_t updated_at;
} MemoryBlock;

/* Create a memory block. Returns row id (>0) or -1 on error.
 * Fails if UNIQUE(agent_name, label) violated. placement: NULL/"" defaults
 * to 'system'. */
int64_t memory_block_create(sqlite3 *db, const char *agent_name, const char *label,
                            const char *description, const char *value, int char_limit,
                            const char *placement);

/* Get a memory block by agent_name + label. Returns NULL if not found. Caller frees. */
MemoryBlock *memory_block_get(sqlite3 *db, const char *agent_name, const char *label);

/* List all memory blocks for an agent. Caller frees with memory_block_list_free. Sets *count. */
MemoryBlock *memory_block_list(sqlite3 *db, const char *agent_name, int *count);

/* Update block value. Enforces char_limit. Returns 0 on success, -1 on error. */
int memory_block_set_value(sqlite3 *db, const char *agent_name, const char *label, const char *value);

/* Free a single MemoryBlock. */
void memory_block_free(MemoryBlock *mb);

/* Free a MemoryBlock array. */
void memory_block_list_free(MemoryBlock *list, int count);

/* Seed memory blocks from agent JSON config (json_each over $.memory_blocks). */
void memory_blocks_seed(sqlite3 *db, const char *agent_name, const char *agent_json_str);

/* A numbered entry inside a memory block. */
typedef struct {
    int pos;       /* 1-based display number, contiguous within the block */
    char *text;
} MemoryEntry;

/* List entries of a block ordered by pos. Caller frees with memory_entries_free.
 * Sets *count. Returns NULL when the block has no entries (count 0). */
MemoryEntry *memory_entries_list(sqlite3 *db, const char *agent_name,
                                 const char *block_label, int *count);
void memory_entries_free(MemoryEntry *list, int count);

/* Append an entry. Returns its new pos (>=1) or -1 on error. */
int memory_entry_add(sqlite3 *db, const char *agent_name,
                     const char *block_label, const char *text);

/* Append a memory entry only if (sum of existing entry lengths + new text)
 * fits char_limit and the block is writable. Returns 1 if inserted, 0 if the
 * guard rejected it (over limit / read-only / missing), -1 on error. */
int memory_entry_add_guarded(sqlite3 *db, const char *agent_name,
                             const char *block_label, const char *text);

/* Replace the text of the entry at the given number/pos.
 * Returns 0 on success, -1 if no entry has that number. */
int memory_entry_set(sqlite3 *db, const char *agent_name,
                     const char *block_label, int number, const char *text);

/* Delete entries whose pos is in numbers[0..n_numbers). After deletion,
 * renumber the remaining entries contiguously (1..N). Returns count deleted. */
int memory_entries_delete(sqlite3 *db, const char *agent_name,
                          const char *block_label, const int *numbers, int n_numbers);

/* Channel→agent binding. Returns heap-allocated agent_name or NULL. */
char *db_channel_binding_get(sqlite3 *db, const char *channel_type, const char *channel_id);

/* Sum cost_nano for all entries in a session. Returns total nanodollars. */
int64_t session_cost(sqlite3 *db, int64_t session_id);

/* Rate limiting — returns 1 if under limit (ok to proceed), 0 if exceeded */
int rate_limit_check(sqlite3 *db, const char *provider_name);

/* Crash recovery, owner-scoped: reclaims only dead-owned stale sessions (owner
 * absent from the processes registry) — any live instance may call it (startup
 * and periodically) without stomping a live peer's in-flight sessions. Resets
 * them to idle and reconciles orphaned tool_calls / llm_jobs / approvals.
 * Returns 0 on success. */
int db_recover_stale_sessions(sqlite3 *db);

/* Prune consumed inbox rows older than config 'inbox_retention_sec'. */
void db_prune_inbox(sqlite3 *db);

/* Prune terminal channel_outbox rows older than config 'outbox_retention_sec'. */
void db_prune_outbox(sqlite3 *db);

/* Best-effort WAL truncate checkpoint (call periodically from housekeeping). */
void db_wal_checkpoint(sqlite3 *db);

/* Free MB on the filesystem holding the main DB, or -1 if unmeasurable. */
long db_free_mb(sqlite3 *db);

/* Scalar query helpers — single int64 bind on param 1, read column 0.
 * db_scalar_i64: returns column 0 as int64 or dflt if no row / prepare fails.
 * db_scalar_text: returns strdup of column 0 text or NULL. Caller frees. */
int64_t db_scalar_i64(sqlite3 *db, const char *sql, int64_t arg, int64_t dflt);
char *db_scalar_text(sqlite3 *db, const char *sql, int64_t arg);

#endif
