#ifndef CCLAW_APPROVAL_H
#define CCLAW_APPROVAL_H

#include <stdint.h>
#include <sqlite3.h>

typedef enum { APPROVAL_DENY, APPROVAL_ONCE, APPROVAL_ALWAYS } ApprovalDecision;

typedef struct {
    int64_t id;
    int64_t session_id;
    char *tool_call_id;
    char *tool_name;
    char *action;
    char *args_json;
    /* args_hash is an audit/dedup fingerprint only, never used as a security check. */
    char *args_hash;
    char *resolve;  /* "rerun" or "apply" — how approval is acted on */
    char *state;
    char *decided_via;
    int64_t requested_at;
    int64_t expires_at;
} Approval;

/* Insert a pending approval. Computes args_hash from args_json (FNV-1a hex).
 * resolve is "rerun" (re-execute frozen call) or "apply" (invoke tool's apply handler).
 * Returns the new row id (>0) or -1 on error. */
int64_t approval_create(sqlite3 *db, int64_t session_id, const char *tool_call_id,
                        const char *tool_name, const char *action,
                        const char *args_json, const char *resolve);

/* Get the pending approval for a session. Returns heap-allocated Approval or NULL. */
Approval *approval_get_pending(sqlite3 *db, int64_t session_id);

/* Get the most recent approval for a (session_id, tool_call_id) pair (any
 * state). Used by the dispatch gate's re-entrancy check (§7a). Scoped to the
 * session because tool_call_ids are model-supplied and not globally unique.
 * Returns heap-allocated or NULL. */
Approval *approval_get_for_tool_call(sqlite3 *db, int64_t session_id,
                                     const char *tool_call_id);

/* Consume an 'approved' approval (approved → consumed), making a once-approval
 * single-use. Returns 0 if a row transitioned, -1 otherwise (already consumed,
 * not approved, or missing). */
int approval_consume(sqlite3 *db, int64_t id);

/* Resolve an approval (approve or deny). Returns heap-allocated resolved Approval or NULL. */
Approval *approval_resolve(sqlite3 *db, int64_t id, int approved, const char *decided_via);

/* Return malloc'd array of expired pending approval ids. Caller frees. Sets *out_count. */
int64_t *approval_list_expired(sqlite3 *db, int *out_count);

/* Free an Approval struct. */
void approval_free(Approval *a);

#endif
