#ifndef CCLAW_RESOLVE_H
#define CCLAW_RESOLVE_H

#include <stdint.h>
#include "approval.h"

/* The approval resolution flow (resolve.c): prompting the approver, settling
 * the decision, applying grants, delivering late decisions. Process-coupled
 * (proc accessors, run_advance re-entry, CLI/daemon prompting) — the db-pure
 * approval row model lives in approval.c. */

/* Single entry point for approval resolution.
 * grant_expires_at: 0 for a permanent grant (normal path); a future unix
 * timestamp to make an APPROVAL_ALWAYS "apply" grant self-expire (used by
 * --auto-approve, which never leaves durable config behind). Ignored for
 * "rerun"-style approvals. */
void resolve_approval(int64_t approval_id, ApprovalDecision decision, const char *decided_via,
                      int64_t grant_expires_at);

/* Prompt the approver for the session's pending approval: CLI prompt (or
 * --auto-approve / non-tty auto-decision, recorded as deferred), daemon
 * channel outbox prompt, or fail-closed auto-deny when there is no approver.
 * Called from the dispatch gate when a call parks. */
void handle_approval_park(int64_t session_id);

/* Apply the auto-decision handle_approval_park recorded, once the dispatch
 * loop has released its CAS claim on the parked tool_call — resolving inside
 * the park would read as post-window and orphan the call (see resolve.c).
 * Called by run_advance after the dispatch loop returns. */
void approval_flush_deferred(void);

#endif
