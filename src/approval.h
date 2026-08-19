#ifndef CCLAW_APPROVAL_H
#define CCLAW_APPROVAL_H

/* Approval records and their lifecycle — the parked decisions a human (or
 * --auto-approve) resolves. approval_create parks one; approval_resolve
 * settles it. Owns the approvals table's row shape.
 */

#include <stddef.h>
#include <stdint.h>
#include <sqlite3.h>

typedef enum { APPROVAL_DENY, APPROVAL_ONCE, APPROVAL_ALWAYS } ApprovalDecision;

/* approvals.park_reason — WHY a row parked. tool_name says WHAT parked; these
 * two values are the whole vocabulary. 'sensitive_target' is the trust.md
 * rule-1 overlay: per-call, never satisfiable by a standing grant, ALWAYS
 * coerced to ONCE. Everything else — gated tools, apply-style tool documents
 * — is an ordinary approval_required park. */
#define APPROVAL_PARK_REQUIRED  "approval_required"
#define APPROVAL_PARK_SENSITIVE "sensitive_target"

typedef struct {
    int64_t id;
    int64_t session_id;
    char *tool_call_id;
    char *tool_name;
    char *park_reason;
    char *args_json;
    char *resolve;  /* "rerun" or "apply" — how approval is acted on */
    char *state;
    char *decided_via;
    int64_t requested_at;
    int64_t expires_at;
} Approval;

/* The two deadline knobs, read from the config registry (falling back to the
 * registry default). approval_timeout_seconds is the park expiry approval_create
 * stamps; approval_block_seconds is the short window a turn blocks before the
 * sweep unparks it, clamped to the timeout so it can never outlast it. */
/* True when this row parked because it targets a sensitive-labeled target
 * (park_reason = APPROVAL_PARK_SENSITIVE) — per-call authority that no
 * standing grant satisfies and no ticket transfers. */
int approval_is_sensitive(const Approval *a);

int approval_timeout_seconds(sqlite3 *db);
int approval_block_seconds(sqlite3 *db);

/* Per-session block window: 0 when the session is pinned to a route on an
 * ambient channel (never freeze a passively-listened room), else the global
 * approval_block_seconds. A 0 window is served inline at park time, so a
 * session that resolves to 0 never reaches the sweep at all. */
int approval_block_seconds_for_session(sqlite3 *db, int64_t session_id);

/* Render the shared background notice ("approval N requested...") into buf. */
void approval_background_notice(int64_t approval_id, char *buf, size_t len);

/* Insert a pending approval.
 * resolve is "rerun" (re-execute frozen call) or "apply" (invoke tool's apply handler).
 * Returns the new row id (>0) or -1 on error. */
int64_t approval_create(sqlite3 *db, int64_t session_id, const char *tool_call_id,
                        const char *tool_name, const char *park_reason,
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

/* Dedupe lookup: a pending rerun approval in this session asking for the same
 * capability (byte-identical args). Returns the row id or 0 — the gate
 * answers the duplicate call inline instead of parking a second row. */
int64_t approval_find_pending_match(sqlite3 *db, int64_t session_id,
                                    const char *park_reason, const char *tool_name,
                                    const char *args_json);

/* Ticket transfer: an approved, unconsumed, unexpired rerun approval whose
 * frozen call could no longer use it (decided post-window) covers the next
 * call asking for the same capability — once. On match the row is consumed
 * (CAS) and its id returned; 0 when nothing transfers. Callers must exclude
 * sensitivity parks (per-call by trust.md rule 1). */
int64_t approval_take_ticket(sqlite3 *db, int64_t session_id,
                             const char *park_reason, const char *tool_name,
                             const char *args_json);

/* Count this session's pending approvals; when buf is non-NULL, format their
 * ids into it ("#4 #7 #9", truncating silently). Returns the count. */
int approval_pending_ids(sqlite3 *db, int64_t session_id, char *buf, size_t cap);

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
    APPROVAL_PW_APPLY_FAILED,     /* human approved, system apply failed —
                                     NOT a denial; safe to re-request once */
    APPROVAL_PW_EXPIRED,
} ApprovalPostWindow;

/* Deliver a late (post-block-window) approval decision as a new inbox turn.
 * Pure delivery: composes the message from the approval + outcome and inserts
 * it into the session's inbox. Does NOT mutate session/tool_call state or apply
 * grants (the caller does that). Returns the inbox row id (>0) or -1.
 * detail (nullable): appended to APPLY_GRANTED/APPLY_FAILED messages — the
 * apply receipt or the failing step. */
int64_t approval_deliver_postwindow(sqlite3 *db, const Approval *a,
                                    ApprovalPostWindow outcome,
                                    const char *detail);

/* decided_via ("channel:<ch>:<sender>", "channel:<ch>", "cli", "auto:...") →
 * plain-language provenance for a receipt: " (<sender>, via <channel>)",
 * " (via <channel>)", or "" when there is no usable token. */
void approval_decider_phrase(const char *via, char *buf, size_t len);

/* One-line re-read of current effective state for the sections the approval's
 * request touched (models list, grants) — read from the DB, never echoed from
 * the request. Terminal receipts (deny, expiry) end with it so the model
 * cannot confabulate what was applied. Always writes something. */
void approval_state_restatement(sqlite3 *db, const Approval *a,
                                char *buf, size_t len);

/* Render a human-readable markdown summary of an approval for the approver's
 * prompt — never the raw args_json blob. Returns a heap string, caller frees. */
char *approval_format_summary(sqlite3 *db, const Approval *a);

/* Free an Approval struct. */
void approval_free(Approval *a);

#endif
