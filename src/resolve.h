#ifndef CCLAW_RESOLVE_H
#define CCLAW_RESOLVE_H

#include <stdint.h>
#include "approval.h"

/* Single entry point for approval resolution (lives in main.c). */
void resolve_approval(int64_t approval_id, ApprovalDecision decision, const char *decided_via);

#endif
