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

/* Apply an approved request_changes document (args_json holds the parked
 * approval args; the document lives at $.changes). Applies grants, config
 * values, and the provider upsert inside one savepoint — all-or-nothing.
 * Shared by apply_grant (main.c) and admin grant-from-history (admin_api.c).
 * Returns 0 iff every line applied. detail_out (nullable): malloc'd receipt —
 * on success what is now in effect (verified by re-read), on failure which
 * step failed. Caller frees. */
int request_config_changes_apply(sqlite3 *db, const char *agent,
                                 const char *args_json, int64_t expires_at,
                                 char **detail_out);

#endif
