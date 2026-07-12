#ifndef CCLAW_TOOL_EXTENSION_H
#define CCLAW_TOOL_EXTENSION_H

#include "tools.h"
#include "sqlite3.h"

/* Lifecycle tools for the declarative extension system:
 *   extension_promote {name}  — copy a workspace draft into the shared store and
 *                               ingest its declarations (a real, owned tool).
 *   extension_publish {name}  — flip the single publish flag (owner only).
 *   extension_attach  {name}  — attach a visible extension to the calling agent.
 *   extension_list            — list extensions visible to the calling agent.
 *
 * These run inline (in the trusted parent process), so — like launch_agent and
 * request_config — they apply directly via ctx->db. No sandboxed tool child
 * ever writes cclaw.db; the daemon parent remains the sole writer. */
typedef struct {
    sqlite3 *db;
    const char *agent_name;   /* promoter/owner / attaching agent */
    const char *workspace;    /* drafts live in <workspace>/extensions/<name> */
    int64_t session_id;                /* set by dispatcher before each call */
    const char *current_tool_call_id;  /* set by dispatcher before each call */
} ToolExtensionCtx;

/* Register extension_promote / extension_publish / extension_attach /
 * extension_list against reg. Returns 0 on success. */
int tool_extension_register(ToolRegistry *reg, ToolExtensionCtx *ctx);

/* "adds N tools, M hooks, ..." enumeration of a bundle's manifest for the
 * promote approval prompt. Heap; caller frees. */
char *extension_manifest_enumerate(sqlite3 *db, const char *bundle_dir);

#endif
