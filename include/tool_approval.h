#ifndef CCLAW_TOOL_APPROVAL_H
#define CCLAW_TOOL_APPROVAL_H

#include "tools.h"
#include "sqlite3.h"

/* Context for approval_request tool */
typedef struct {
    sqlite3 *db;
    int64_t session_id;
    const char *agent_name;
} ToolApprovalCtx;

/* Register approval_request tool. */
int tool_approval_register(ToolRegistry *reg, ToolApprovalCtx *ctx);

#endif
