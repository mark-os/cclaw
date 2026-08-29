#ifndef CCLAW_TOOL_BOOTSTRAP_H
#define CCLAW_TOOL_BOOTSTRAP_H

/* The create_agent / update_agent tools — agent self-configuration, run
 * inline in the trusted parent (they write the agents table directly).
 */

#include "tools.h"
#include "sqlite3.h"

/* Context for bootstrap tools (create_agent, update_agent) */
typedef struct {
    sqlite3 *db;
    int64_t session_id;
    const char *agent_name;
    const char *current_tool_call_id;  /* set by dispatcher before each call */
} ToolBootstrapCtx;

/* Register create_agent / update_agent tools. */
int tool_create_agent_register(ToolRegistry *reg, ToolBootstrapCtx *ctx);
int tool_update_agent_register(ToolRegistry *reg, ToolBootstrapCtx *ctx);

/* install_update: offer a noticed release; approval runs the update rails. */
int tool_install_update_register(ToolRegistry *reg, ToolBootstrapCtx *ctx);

#endif
