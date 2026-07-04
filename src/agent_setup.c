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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int agent_setup_init(AgentSetup *setup, sqlite3 *db, int64_t session_id,
                     const Config *cfg, const char *agent_name, int mode) {
    memset(setup, 0, sizeof(*setup));
    tools_init(&setup->reg);

    /* Load capabilities from grants table */
    agent_caps_load(db, agent_name, &setup->caps);

    /* The shell egress proxy is per-call: each shell_exec stands up its own
     * proxy in the --run-tool broker child. The daemon holds no proxy socket. */

    /* V88: Collect secrets from env, clear from process env */
    setup->secrets = shell_secrets_collect(&setup->secret_count);

    /* Sandbox profile: env override (-y sets CCLAW_SANDBOX_PROFILE=host), else agents table */
    const char *sandbox_profile = getenv("CCLAW_SANDBOX_PROFILE");
    char trust_buf[32] = {0};
    if (!sandbox_profile) {
        sqlite3_stmt *tl_stmt;
        if (sqlite3_prepare_v2(db,
                "SELECT sandbox_profile FROM agents WHERE name=?", -1, &tl_stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(tl_stmt, 1, agent_name, -1, SQLITE_STATIC);
            if (sqlite3_step(tl_stmt) == SQLITE_ROW) {
                const char *v = (const char *)sqlite3_column_text(tl_stmt, 0);
                if (v) { snprintf(trust_buf, sizeof(trust_buf), "%s", v); sandbox_profile = trust_buf; }
            }
            sqlite3_finalize(tl_stmt);
        }
    }
    /* Export so forked children (js_eval) inherit it */
    if (sandbox_profile)
        setenv("CCLAW_SANDBOX_PROFILE", sandbox_profile, 1);

    /* Shell — pass proxy socket path */
    tool_shell_register(&setup->reg, cfg->shell_timeout, cfg->workspace);

    /* Trust-derived sandbox profile, filled once and embedded in every tool ctx.
     * The grant-path fields are bound just below (and rebound on cap refresh). */
    SandboxProfile profile = {0};
    sandbox_profile_resolve(sandbox_profile, &profile);
    profile.read_paths = setup->caps.read_paths;
    profile.read_path_count = setup->caps.read_count;
    profile.write_paths = setup->caps.write_paths;
    profile.write_path_count = setup->caps.write_count;

    /* Inject proxy sock path + secrets + the shared profile into shell config */
    ToolEntry *shell_entry = tools_lookup(&setup->reg, "shell_exec");
    if (shell_entry && shell_entry->user_data) {
        ShellConfig *sc = (ShellConfig *)shell_entry->user_data;
        sc->allowed_hosts = setup->caps.hosts;       /* per-call egress proxy allowlist (data, no DB) */
        sc->allowed_host_count = setup->caps.host_count;
        sc->secrets = setup->secrets;
        sc->secret_count = setup->secret_count;
        sc->cwd_path = getenv("CCLAW_PATH");  /* T276/V22a: CWD rw in CLI mode */
        sc->db_path = cfg->db_path;           /* mask .cclaw_key + db ciphertext from shell children */
        sc->sb = profile;
    }

    /* File tools — forked sandbox path shares the profile with shell */
    setup->file_read_ctx.workspace = cfg->workspace;
    setup->file_read_ctx.cwd_path = getenv("CCLAW_PATH");
    setup->file_read_ctx.db_path = cfg->db_path;
    setup->file_read_ctx.sb = profile;
    tool_file_read_register(&setup->reg, &setup->file_read_ctx);
    tool_file_write_register(&setup->reg, &setup->file_read_ctx);
    tool_file_list_register(&setup->reg, &setup->file_read_ctx);
    tool_file_find_register(&setup->reg, &setup->file_read_ctx);
    tool_file_edit_register(&setup->reg, &setup->file_read_ctx);
    tool_file_grep_register(&setup->reg, &setup->file_read_ctx);

    /* JS eval (SBX_JS broker): mirrors web/shell — qjs runs in-process in the
     * fork+execve --run-tool child, egress via per-hop proxy decide(). */
    setup->js_eval_ctx.allowed_hosts = setup->caps.hosts;
    setup->js_eval_ctx.allowed_hosts_count = setup->caps.host_count;
    setup->js_eval_ctx.host_mode = (sandbox_profile && strcmp(sandbox_profile, "host") == 0) ? 1 : 0;
    setup->js_eval_ctx.sandbox_profile = sandbox_profile;
    setup->js_eval_ctx.workspace = cfg->workspace;
    setup->js_eval_ctx.cwd_path = getenv("CCLAW_PATH");
    setup->js_eval_ctx.db_path = cfg->db_path;
    setup->js_eval_ctx.sb = profile;
    tool_js_eval_register(&setup->reg, &setup->js_eval_ctx);

    /* web_fetch — sandboxed broker (SBX_WEB), egress via per-hop proxy decide().
     * Shares the profile + workspace + host grants with shell. */
    setup->web_ctx.workspace = cfg->workspace;
    setup->web_ctx.cwd_path = getenv("CCLAW_PATH");
    setup->web_ctx.db_path = cfg->db_path;
    setup->web_ctx.allowed_hosts = setup->caps.hosts;
    setup->web_ctx.allowed_host_count = setup->caps.host_count;
    setup->web_ctx.host_mode = (sandbox_profile && strcmp(sandbox_profile, "host") == 0) ? 1 : 0;
    setup->web_ctx.sb = profile;
    tool_web_fetch_register(&setup->reg, &setup->web_ctx);

    /* db_query */
    tool_db_query_register(&setup->reg, db);

    /* Memory tools */
    setup->mem_ctx.db = db;
    setup->mem_ctx.agent_name = (char *)agent_name;
    tool_memory_register(&setup->reg, &setup->mem_ctx);

    /* JS persistent runtime */
    setup->js_rt = js_runtime_create();
    if (setup->js_rt && setup->caps.host_count > 0)
        js_runtime_set_hosts(setup->js_rt, setup->caps.hosts, setup->caps.host_count);

    /* Extension hook context + DB-driven hook load. Extension *tools* load from
     * the DB after tools_sync_to_db (below) so the builtin sync never clobbers
     * extension rows. No JS is evaluated at load time. */
    extension_ctx_init(&setup->ext_ctx, setup->js_rt);
    extension_load_hooks(&setup->ext_ctx, db, agent_name);

    /* T274/V120: request_config — CLI inline tool/host/rename */
    setup->req_cfg_ctx.db = db;
    setup->req_cfg_ctx.agent_name = agent_name;
    setup->req_cfg_ctx.session_id = session_id;
    setup->req_cfg_ctx.agents_dir = NULL;  /* set by caller if known */
    tool_request_config_register(&setup->reg, &setup->req_cfg_ctx);

    /* Read-only config introspection — always available */
    setup->search_cfg_ctx.db = db;
    setup->search_cfg_ctx.agent_name = agent_name;
    tool_search_config_register(&setup->reg, &setup->search_cfg_ctx);

    /* Bootstrap tools — available in both CLI and daemon (DB config only) */
    setup->bootstrap_ctx.db = db;
    setup->bootstrap_ctx.session_id = session_id;
    setup->bootstrap_ctx.agent_name = (char *)agent_name;
    tool_configure_provider_register(&setup->reg, &setup->bootstrap_ctx);
    tool_configure_channel_register(&setup->reg, &setup->bootstrap_ctx);
    tool_create_agent_register(&setup->reg, &setup->bootstrap_ctx);

    /* Extension lifecycle — inline tools (apply in-process via ctx->db), so
     * available in both CLI and daemon like the bootstrap tools. */
    setup->ext_tool_ctx.db = db;
    setup->ext_tool_ctx.agent_name = agent_name;
    setup->ext_tool_ctx.workspace = cfg->workspace;
    tool_extension_register(&setup->reg, &setup->ext_tool_ctx);

    /* Cron tools — DB CRUD only; daemon fires jobs, CLI just manages them. */
    setup->cron_ctx.db = db;
    setup->cron_ctx.session_id = session_id;
    setup->cron_ctx.agent_name = agent_name;
    tool_cron_register(&setup->reg, &setup->cron_ctx);

    /* secret_create — inline in parent process (needs the db handle to write
     * the secrets table directly; the value never crosses into args/result). */
    setup->secret_create_ctx.db = db;
    tool_secret_create_register(&setup->reg, &setup->secret_create_ctx);

    /* Agent launch + status check. Available in both CLI and daemon: both run
     * the same event loop with a wake pipe, so a spawned child advances and a
     * parent waiting on it resumes in either. launch_agent is gated by depth
     * (a leaf agent at the ceiling can't spawn). */
    (void)mode;
    setup->launch_ctx.db = db;
    setup->launch_ctx.session_id = session_id;
    int depth = session_get_depth(db, session_id);
    if (depth < agent_max_depth(db))
        tool_launch_agent_register(&setup->reg, &setup->launch_ctx);
    tool_check_session_register(&setup->reg, &setup->launch_ctx);
    tool_check_approval_register(&setup->reg, &setup->launch_ctx);

    /* Persist builtin schemas (builtin=1), then materialize this agent's
     * extension tools from the DB join — order matters: the sync must see only
     * builtins. */
    tools_sync_to_db(&setup->reg, db);
    tools_load_extension_tools(&setup->reg, db, agent_name, &setup->js_eval_ctx);
    return 0;
}

void agent_setup_refresh_caps(AgentSetup *setup, sqlite3 *db, const char *agent) {
    agent_caps_refresh(db, agent, &setup->caps);

    /* Rebind the grant-path pointers in every embedded profile to the new arrays.
     * Only the path half changes on a cap refresh; the trust policy is fixed. */
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

    if (setup->js_rt)
        js_runtime_set_hosts(setup->js_rt, setup->caps.hosts, setup->caps.host_count);
}

void agent_setup_destroy(AgentSetup *setup) {
    extension_ctx_destroy(&setup->ext_ctx);
    js_runtime_destroy(setup->js_rt);
    shell_secrets_free(setup->secrets, setup->secret_count);
    agent_caps_free(&setup->caps);
    tools_free(&setup->reg);
}
