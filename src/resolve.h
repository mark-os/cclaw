#ifndef CCLAW_RESOLVE_H
#define CCLAW_RESOLVE_H

#include <stdint.h>
#include "approval.h"

/* Single entry point for approval resolution (lives in main.c).
 * grant_expires_at: 0 for a permanent grant (normal path); a future unix
 * timestamp to make an APPROVAL_ALWAYS "apply" grant self-expire (used by
 * --auto-approve, which never leaves durable config behind). Ignored for
 * "rerun"-style approvals. */
void resolve_approval(int64_t approval_id, ApprovalDecision decision, const char *decided_via,
                      int64_t grant_expires_at);

#endif
