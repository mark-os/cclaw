#ifndef CCLAW_TOOL_REQUEST_CONFIG_H
#define CCLAW_TOOL_REQUEST_CONFIG_H

#include "tools.h"
#include "sqlite3.h"

/* Context for request_config tool (inline in parent process). */
typedef struct {
    sqlite3 *db;
    const char *agent_name;
    int64_t session_id;
    char *agents_dir;  /* e.g. "/home/user/.cclaw/agents" */
    const char *current_tool_call_id;  /* set by dispatcher before each call */
} RequestConfigCtx;

int tool_request_config_register(ToolRegistry *reg, RequestConfigCtx *ctx);

#endif
