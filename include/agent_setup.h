#ifndef CCLAW_AGENT_SETUP_H
#define CCLAW_AGENT_SETUP_H

#include "tools.h"
#include "tool_js.h"
#include "tool_file.h"
#include "tool_shell.h"
#include "tool_memory.h"
#include "tool_bootstrap.h"
#include "tool_agent.h"
#include "tool_request_config.h"
#include "tool_search_config.h"
#include "http_policy.h"
#include "proxy.h"
#include "extension.h"
#include "config.h"
#include "agent_config.h"
#include "db.h"

/* T206: Shared agent tool setup context — holds all tool contexts that need
 * to outlive the setup call (caller owns lifetime). */
typedef struct {
    ToolRegistry reg;
    JsSessionRuntime *js_rt;
    /* Live-refreshable capability arrays from grants table */
    AgentCaps caps;
    /* Contexts that tools reference (must stay alive during agent_run) */
    FileReadCtx file_read_ctx;
    JsEvalCtx js_eval_ctx;
    HttpPolicy web_policy;
    ToolMemoryCtx mem_ctx;
    /* Daemon-mode only contexts */
    ToolBootstrapCtx bootstrap_ctx;
    AgentLaunchCtx launch_ctx;
    RequestConfigCtx req_cfg_ctx;
    SearchConfigCtx search_cfg_ctx;
    /* V83: credential proxy for shell children */
    ProxyContext proxy_ctx;
    /* V88: secrets for shell injection + masking */
    ShellSecret *secrets;
    size_t secret_count;
    /* T256: extension hooks context */
    ExtensionCtx ext_ctx;
} AgentSetup;

/* Mode flags for agent_setup_init */
#define AGENT_SETUP_CLI     0   /* standalone CLI: no daemon-dependent tools */
#define AGENT_SETUP_DAEMON  1   /* daemon-forked agent: all tools */

/* Initialize tool registry with appropriate tools for the mode.
 * cli_mode=AGENT_SETUP_CLI excludes spawn_agent, cron, approval, bootstrap.
 * cli_mode=AGENT_SETUP_DAEMON includes all tools.
 * Returns 0 on success. Caller must call agent_setup_destroy() when done. */
int agent_setup_init(AgentSetup *setup, sqlite3 *db, int64_t session_id,
                     const Config *cfg, const char *agent_name, int mode);

/* Destroy setup (free registry, JS runtime, caps). */
void agent_setup_destroy(AgentSetup *setup);

#endif
