#ifndef CCLAW_APPROVAL_H
#define CCLAW_APPROVAL_H

/* Approval records and their lifecycle — the parked decisions a human (or
 * --auto-approve) resolves. approval_create parks one; approval_resolve
 * settles it. Owns the approvals table's row shape.
 */

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
    char *resolve;  /* "rerun" or "apply" — how approval is acted on */
    char *state;
    char *decided_via;
    int64_t requested_at;
    int64_t expires_at;
} Approval;

/* Insert a pending approval.
 * resolve is "rerun" (re-execute frozen call) or "apply" (invoke tool's apply handler).
 * Returns the new row id (>0) or -1 on error. */
int64_t approval_create(sqlite3 *db, int64_t session_id, const char *tool_call_id,
                        const char *tool_name, const char *action,
                        const char *args_json, const char *resolve);

/* Get the pending approval for a session. Returns heap-allocated Approval or NULL. */
Approval *approval_get_pending(sqlite3 *db, int64_t session_id);

/* Get the oldest pending approval anywhere in the session subtree rooted at
 * root_session_id (the session itself plus every descendant reachable via
 * parent_session_id, e.g. a launch_agent sub-agent). Lets one CLI approver
 * answer a sub-agent's park, not just the root session's own. Returns
 * heap-allocated Approval or NULL. */
Approval *approval_get_pending_subtree(sqlite3 *db, int64_t root_session_id);

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

/* Return malloc'd array of expired pending approval ids, owner-scoped: only
 * approvals on sessions owned by `me`, unowned, or dead-owned (owner absent
 * from the processes registry). `me` may be NULL/"" to mean "no live owner".
 * Caller frees. Sets *out_count. */
int64_t *approval_list_expired(sqlite3 *db, const char *me, int *out_count);

/* Return malloc'd array of pending approval ids past the short block window
 * (requested_at + block_sec < now) whose session is still awaiting_approval,
 * owner-scoped to `me` exactly like approval_list_expired. Caller frees. */
int64_t *approval_list_block_due(sqlite3 *db, int block_sec, const char *me, int *out_count);

/* Post-window delivery outcomes — how a late approval decision is described in
 * the inbox follow-up turn (see approval_deliver_postwindow). */
typedef enum {
    APPROVAL_PW_RERUN_APPROVED,  /* gated tool call: notify-only, re-issue if needed */
    APPROVAL_PW_RERUN_DENIED,
    APPROVAL_PW_APPLY_GRANTED,    /* capability grant applied by the caller */
    APPROVAL_PW_APPLY_DENIED,
    APPROVAL_PW_EXPIRED,
} ApprovalPostWindow;

/* Deliver a late (post-block-window) approval decision as a new inbox turn.
 * Pure delivery: composes the message from the approval + outcome and inserts
 * it into the session's inbox. Does NOT mutate session/tool_call state or apply
 * grants (the caller does that). Returns the inbox row id (>0) or -1. */
int64_t approval_deliver_postwindow(sqlite3 *db, const Approval *a, ApprovalPostWindow outcome);

/* Free an Approval struct. */
void approval_free(Approval *a);

#endif
