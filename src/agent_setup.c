#define _POSIX_C_SOURCE 200809L
#include "agent_setup.h"
#include "extension.h"
#include "tool_shell.h"
#include "tool_web_fetch.h"
#include "tool_db_query.h"
#include "tool_cron.h"
#include "tool_request_config.h"
#include "sandbox.h"
#include "log.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int agent_setup_init(AgentSetup *setup, sqlite3 *db, int64_t session_id,
                     const Config *cfg, const char *agent_name) {
    memset(setup, 0, sizeof(*setup));
    tools_init(&setup->reg);

    /* Capability arrays (caps) are not loaded here: agent_setup_refresh_caps
     * at the bottom loads them from the grants table, and the dispatch loop
     * re-runs it before every tool batch — grants in the DB are the only
     * durable authority; the arrays are a per-dispatch snapshot. */

    /* The shell egress proxy is per-call: each shell_exec stands up its own
     * proxy in the --run-tool broker child. The daemon holds no proxy socket. */

    /* Collect secrets from env, clear from process env */
    setup->secrets = shell_secrets_collect(&setup->secret_count);

    /* Containment config (sandbox_profile, shell_path) is not resolved here:
     * agent_setup_refresh_caps re-reads it from the agents table before every
     * tool batch, same as grants. Only capture whether an operator env
     * override exists now (--trust-host sets CCLAW_SANDBOX_PROFILE=host) — refresh
     * setenv's for children, so a later getenv can't distinguish the two. */
    const char *ev = getenv("CCLAW_SANDBOX_PROFILE");
    if (ev) {
        snprintf(setup->sandbox_profile_buf, sizeof(setup->sandbox_profile_buf), "%s", ev);
        setup->sandbox_profile_env = 1;
    }
    ev = getenv("CCLAW_SHELL_PATH");
    if (ev) {
        snprintf(setup->shell_path_buf, sizeof(setup->shell_path_buf), "%s", ev);
        setup->shell_path_env = 1;
    }

    /* Workspace root for per-agent derivation. agent_dir_resolve with a NULL
     * workspace gives <db_dir>/agents — deliberately not derived from
     * cfg->workspace, whose default already points *inside* the tree at
     * agents/default/workspace. */
    setup->workspace_pinned = cfg->workspace_explicit;
    if (!setup->workspace_pinned)
        agent_dir_resolve(NULL, cfg->db_path, setup->agents_dir,
                          sizeof(setup->agents_dir));

    /* Shell — shell_path bound by refresh below, like the caps */
    tool_shell_register(&setup->reg, cfg->shell_timeout, cfg->workspace, NULL);

    /* Inject secrets into shell config; the sandbox profile is bound by
     * agent_setup_refresh_caps (and per-dispatch reloads keep it fresh) */
    ToolEntry *shell_entry = tools_lookup(&setup->reg, "shell_exec");
    if (shell_entry && shell_entry->user_data) {
        ShellConfig *sc = (ShellConfig *)shell_entry->user_data;
        sc->secrets = setup->secrets;
        sc->secret_count = setup->secret_count;
        sc->cwd_path = getenv("CCLAW_PATH");  /* CWD rw in CLI mode */
        sc->db_path = cfg->db_path;           /* mask .cclaw_key + db ciphertext from shell children */
    }

    /* File tools — forked sandbox path shares the refreshed profile with shell */
    setup->file_read_ctx.workspace = cfg->workspace;
    setup->file_read_ctx.cwd_path = getenv("CCLAW_PATH");
    setup->file_read_ctx.db_path = cfg->db_path;
    tool_file_read_register(&setup->reg, &setup->file_read_ctx);
    tool_file_write_register(&setup->reg, &setup->file_read_ctx);
    tool_file_list_register(&setup->reg, &setup->file_read_ctx);
    tool_file_find_register(&setup->reg, &setup->file_read_ctx);
    tool_file_edit_register(&setup->reg, &setup->file_read_ctx);
    tool_file_grep_register(&setup->reg, &setup->file_read_ctx);

    /* JS eval (SBX_JS broker): mirrors web/shell — qjs runs in-process in the
     * fork+execve --run-tool child, egress via per-hop proxy decide(). */
    setup->js_eval_ctx.workspace = cfg->workspace;
    setup->js_eval_ctx.cwd_path = getenv("CCLAW_PATH");
    setup->js_eval_ctx.db_path = cfg->db_path;
    tool_js_eval_register(&setup->reg, &setup->js_eval_ctx);

    /* web_fetch — sandboxed broker (SBX_WEB), egress via per-hop proxy decide().
     * Shares the profile + workspace + host grants with shell. */
    setup->web_ctx.workspace = cfg->workspace;
    setup->web_ctx.cwd_path = getenv("CCLAW_PATH");
    setup->web_ctx.db_path = cfg->db_path;
    tool_web_fetch_register(&setup->reg, &setup->web_ctx);

    /* db_query */
    tool_db_query_register(&setup->reg, db);

    /* session_condense — registered so it is discoverable under "Requestable
     * Tools", deliberately absent from agent_default_tools: it rewrites the
     * agent's own past, so it is granted, never assumed. */
    setup->condense_ctx.db = db;
    setup->condense_ctx.session_id = session_id;
    snprintf(setup->condense_ctx.agent_name, sizeof(setup->condense_ctx.agent_name),
             "%s", agent_name ? agent_name : "");
    tool_session_condense_register(&setup->reg, &setup->condense_ctx);

    /* Memory tools */
    setup->mem_ctx.db = db;
    setup->mem_ctx.agent_name = (char *)agent_name;
    tool_memory_register(&setup->reg, &setup->mem_ctx);

    /* JS persistent runtime */
    setup->js_rt = js_runtime_create();

    /* Extension hook context + DB-driven hook load. Extension *tools* load from
     * the DB after tools_sync_to_db (below) so the builtin sync never clobbers
     * extension rows. No JS is evaluated at load time. */
    extension_ctx_init(&setup->ext_ctx, setup->js_rt);
    extension_load_hooks(&setup->ext_ctx, db, agent_name);

    /* request_config — CLI inline tool/host/rename */
    setup->req_cfg_ctx.db = db;
    setup->req_cfg_ctx.agent_name = agent_name;
    setup->req_cfg_ctx.session_id = session_id;
    setup->req_cfg_ctx.agents_dir = NULL;  /* set by caller if known */
    tool_request_config_register(&setup->reg, &setup->req_cfg_ctx);

    /* Read-only config introspection — always available */
    setup->search_cfg_ctx.db = db;
    setup->search_cfg_ctx.agent_name = agent_name;
    setup->search_cfg_ctx.session_id = session_id;
    tool_search_config_register(&setup->reg, &setup->search_cfg_ctx);

    /* Provider-catalog search — registered so it is discoverable under
     * "Requestable Tools", deliberately absent from agent_default_tools: it is
     * the one introspection tool that can reach the network. */
    setup->search_models_ctx.db = db;
    tool_search_models_register(&setup->reg, &setup->search_models_ctx);

    /* Bootstrap tools — available in both CLI and daemon (DB config only) */
    setup->bootstrap_ctx.db = db;
    setup->bootstrap_ctx.session_id = session_id;
    setup->bootstrap_ctx.agent_name = (char *)agent_name;
    setup->bootstrap_ctx.current_tool_call_id = NULL;
    tool_create_agent_register(&setup->reg, &setup->bootstrap_ctx);
    tool_update_agent_register(&setup->reg, &setup->bootstrap_ctx);

    /* Extension lifecycle — inline tools (apply in-process via ctx->db), so
     * available in both CLI and daemon like the bootstrap tools. */
    setup->ext_tool_ctx.db = db;
    setup->ext_tool_ctx.agent_name = agent_name;
    setup->ext_tool_ctx.workspace = cfg->workspace;
    tool_extension_register(&setup->reg, &setup->ext_tool_ctx);

    /* Cron tools — DB CRUD only; daemon fires jobs, CLI just manages them.
     * cron_set resolves script paths against the agent's workspace. */
    setup->cron_ctx.db = db;
    setup->cron_ctx.session_id = session_id;
    setup->cron_ctx.workspace = cfg->workspace;
    snprintf(setup->cron_ctx.agent_name, sizeof(setup->cron_ctx.agent_name),
             "%s", agent_name ? agent_name : "");
    tool_cron_register(&setup->reg, &setup->cron_ctx);

    /* secret_create — inline in parent process (needs the db handle to write
     * the secrets table directly; the value never crosses into args/result). */
    setup->secret_create_ctx.db = db;
    tool_secret_create_register(&setup->reg, &setup->secret_create_ctx);

    /* channel_send — outbox insert only (the daemon's runners drain it), so
     * registerable in both modes. Ships ungranted: routes are the allowlist,
     * the grant is the operator's call. Dispatch threads session + agent. */
    setup->chan_send_ctx.db = db;
    setup->chan_send_ctx.db_path = cfg->db_path;
    setup->chan_send_ctx.session_id = session_id;
    snprintf(setup->chan_send_ctx.agent_name, sizeof(setup->chan_send_ctx.agent_name),
             "%s", agent_name ? agent_name : "");
    tool_channel_send_register(&setup->reg, &setup->chan_send_ctx);

    /* Agent launch + status check. Available in both CLI and daemon: both run
     * the same event loop with a wake pipe, so a spawned child advances and a
     * parent waiting on it resumes in either. launch_agent is gated by depth
     * (a leaf agent at the ceiling can't spawn). */
    setup->launch_ctx.db = db;
    setup->launch_ctx.session_id = session_id;
    int depth = session_get_depth(db, session_id);
    if (depth < agent_max_depth(db))
        tool_launch_agent_register(&setup->reg, &setup->launch_ctx);
    tool_check_session_register(&setup->reg, &setup->launch_ctx);

    /* Persist builtin (C tool) schemas, then materialize this agent's
     * extension tools from the DB join — order matters: the sync must see only
     * builtins. */
    tools_sync_to_db(&setup->reg, db);
    tools_load_extension_tools(&setup->reg, db, agent_name, &setup->js_eval_ctx);

    /* Initial caps + containment load and bind — same path the dispatch loop
     * re-runs per batch. */
    agent_setup_refresh_caps(setup, db, agent_name);
    return 0;
}

/* Copy an agents-table column into a setup-owned buffer (empty on no row /
 * NULL) so ctx pointers into it stay valid between refreshes. */
static void agents_column_read(sqlite3 *db, const char *agent, const char *sql,
                               char *buf, size_t buf_sz) {
    buf[0] = '\0';
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, agent, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) snprintf(buf, buf_sz, "%s", v);
    }
    sqlite3_finalize(st);
}

/* Re-read containment config from the agents table (unless an operator env
 * override was present at init) and rebind the resolved profile into every
 * tool ctx. Same freshness contract as the caps: a mid-run update_agent or
 * an agent switch takes effect on the next dispatch, not at restart. */
static void containment_refresh(AgentSetup *setup, sqlite3 *db, const char *agent) {
    if (!setup->sandbox_profile_env)
        agents_column_read(db, agent,
            "SELECT sandbox_profile FROM agents WHERE name=?",
            setup->sandbox_profile_buf, sizeof(setup->sandbox_profile_buf));
    if (!setup->shell_path_env)
        agents_column_read(db, agent,
            "SELECT shell_path FROM agents WHERE name=?",
            setup->shell_path_buf, sizeof(setup->shell_path_buf));

    const char *profile_str =
        setup->sandbox_profile_buf[0] ? setup->sandbox_profile_buf : NULL;
    const char *shell_path =
        setup->shell_path_buf[0] ? setup->shell_path_buf : NULL;

    /* Export so children forked this dispatch inherit the *current* values
     * (js_eval; search_config also reads the shell env). */
    if (profile_str) setenv("CCLAW_SANDBOX_PROFILE", profile_str, 1);
    else unsetenv("CCLAW_SANDBOX_PROFILE");
    if (shell_path) setenv("CCLAW_SHELL_PATH", shell_path, 1);
    else unsetenv("CCLAW_SHELL_PATH");

    /* Trust-derived policy, whole-struct copied into each ctx; the caps
     * rebind that follows overwrites the grant-path half. */
    SandboxProfile profile = {0};
    sandbox_profile_resolve(profile_str, &profile);

    setup->file_read_ctx.sb = profile;

    setup->js_eval_ctx.sb = profile;
    setup->js_eval_ctx.host_mode = profile.sandbox ? 0 : 1;
    setup->js_eval_ctx.sandbox_profile = profile_str;

    setup->web_ctx.sb = profile;
    setup->web_ctx.host_mode = profile.sandbox ? 0 : 1;

    ToolEntry *shell_entry = tools_lookup(&setup->reg, "shell_exec");
    if (shell_entry && shell_entry->user_data) {
        ShellConfig *sc = (ShellConfig *)shell_entry->user_data;
        sc->sb = profile;
        sc->shell_path = shell_path;  /* NULL = /bin/sh (tool_shell.c default) */
    }
}

/* Point every file/shell/js/web ctx at the advancing agent's own workspace.
 * Without this one shared setup left every agent operating in the process
 * -global cfg->workspace (agents/default/workspace), so a daemon-run agent
 * saw an empty directory that was not its own. Created eagerly: the tools
 * fail with "no workspace configured" rather than mkdir on demand. */
static void workspace_refresh(AgentSetup *setup, const char *agent) {
    if (setup->workspace_pinned || !agent || !agent[0]) return;
    if (strchr(agent, '/') || strcmp(agent, "..") == 0) return;

    int n = snprintf(setup->workspace_buf, sizeof(setup->workspace_buf),
                     "%s/%s/workspace", setup->agents_dir, agent);
    if (n <= 0 || (size_t)n >= sizeof(setup->workspace_buf)) {
        setup->workspace_buf[0] = '\0';
        return;
    }
    if (util_mkdir_p(setup->workspace_buf) != 0)
        LOG_WARN_("workspace '%s' could not be created", setup->workspace_buf);

    setup->file_read_ctx.workspace = setup->workspace_buf;
    setup->js_eval_ctx.workspace = setup->workspace_buf;
    setup->web_ctx.workspace = setup->workspace_buf;
    /* Extension drafts live under <workspace>/extensions, so the draft →
     * promote flow has to follow the agent too — a draft written by one agent
     * must not become another's to promote. */
    setup->ext_tool_ctx.workspace = setup->workspace_buf;
    /* cron_set's script paths are workspace-relative, and the file must exist
     * when the job is set — so it has to be the advancing agent's workspace. */
    setup->cron_ctx.workspace = setup->workspace_buf;

    ToolEntry *shell_entry = tools_lookup(&setup->reg, "shell_exec");
    if (shell_entry && shell_entry->user_data)
        ((ShellConfig *)shell_entry->user_data)->workspace = setup->workspace_buf;
}

/* Load the agent's grants into flat arrays and bind them into every tool ctx.
 * Called by the dispatch loop before each tool batch, so the arrays are always
 * a fresh snapshot of the grants table for the *advancing* agent — never a
 * cache that can drift from the DB (expiry, revoke, agent switch). Handlers
 * consume them synchronously in the parent; forked children copy at fork. */
void agent_setup_refresh_caps(AgentSetup *setup, sqlite3 *db, const char *agent) {
    agent_caps_refresh(db, agent, &setup->caps);

    /* Containment first: it whole-struct copies each embedded profile, then
     * the grant-path half below is layered on top. Trust policy is a
     * per-dispatch snapshot too — no longer fixed at init. */
    containment_refresh(setup, db, agent);
    workspace_refresh(setup, agent);
    setup->file_read_ctx.sb.read_paths = setup->caps.read_paths;
    setup->file_read_ctx.sb.read_path_count = setup->caps.read_count;
    setup->file_read_ctx.sb.write_paths = setup->caps.write_paths;
    setup->file_read_ctx.sb.write_path_count = setup->caps.write_count;

    setup->js_eval_ctx.allowed_hosts = setup->caps.hosts;
    setup->js_eval_ctx.allowed_hosts_count = setup->caps.host_count;
    setup->js_eval_ctx.sb.read_paths = setup->caps.read_paths;
    setup->js_eval_ctx.sb.read_path_count = setup->caps.read_count;
    setup->js_eval_ctx.sb.write_paths = setup->caps.write_paths;
    setup->js_eval_ctx.sb.write_path_count = setup->caps.write_count;

    ToolEntry *shell_entry = tools_lookup(&setup->reg, "shell_exec");
    if (shell_entry && shell_entry->user_data) {
        ShellConfig *sc = (ShellConfig *)shell_entry->user_data;
        sc->allowed_hosts = setup->caps.hosts;
        sc->allowed_host_count = setup->caps.host_count;
        sc->sb.read_paths = setup->caps.read_paths;
        sc->sb.read_path_count = setup->caps.read_count;
        sc->sb.write_paths = setup->caps.write_paths;
        sc->sb.write_path_count = setup->caps.write_count;
    }

    setup->web_ctx.allowed_hosts = setup->caps.hosts;
    setup->web_ctx.allowed_host_count = setup->caps.host_count;
    setup->web_ctx.sb.read_paths = setup->caps.read_paths;
    setup->web_ctx.sb.read_path_count = setup->caps.read_count;
    setup->web_ctx.sb.write_paths = setup->caps.write_paths;
    setup->web_ctx.sb.write_path_count = setup->caps.write_count;
}

void agent_setup_destroy(AgentSetup *setup) {
    extension_ctx_destroy(&setup->ext_ctx);
    js_runtime_destroy(setup->js_rt);
    shell_secrets_free(setup->secrets, setup->secret_count);
    agent_caps_free(&setup->caps);
    tools_free(&setup->reg);
}
