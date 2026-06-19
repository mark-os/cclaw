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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int agent_setup_init(AgentSetup *setup, sqlite3 *db, int64_t session_id,
                     const Config *cfg, const char *agent_name, int mode) {
    memset(setup, 0, sizeof(*setup));
    setup->proxy_ctx.listen_fd = -1;  /* fd 0 is stdin — zeroed ctx must not close it */
    tools_init(&setup->reg);

    /* Load capabilities from grants table */
    agent_caps_load(db, agent_name, &setup->caps);

    /* V83: Start credential proxy thread for shell children */
    if (cfg->workspace)
        proxy_start(&setup->proxy_ctx, cfg->workspace, cfg->db_path, agent_name);

    /* V88: Collect secrets from env, clear from process env */
    setup->secrets = shell_secrets_collect(&setup->secret_count);

    /* Trust level: env override (-y sets CCLAW_TRUST_LEVEL=host), else agents table */
    const char *trust_level = getenv("CCLAW_TRUST_LEVEL");
    char trust_buf[32] = {0};
    if (!trust_level) {
        sqlite3_stmt *tl_stmt;
        if (sqlite3_prepare_v2(db,
                "SELECT trust_level FROM agents WHERE name=?", -1, &tl_stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(tl_stmt, 1, agent_name, -1, SQLITE_STATIC);
            if (sqlite3_step(tl_stmt) == SQLITE_ROW) {
                const char *v = (const char *)sqlite3_column_text(tl_stmt, 0);
                if (v) { snprintf(trust_buf, sizeof(trust_buf), "%s", v); trust_level = trust_buf; }
            }
            sqlite3_finalize(tl_stmt);
        }
    }
    /* Export so forked children (js_eval) inherit it */
    if (trust_level)
        setenv("CCLAW_TRUST_LEVEL", trust_level, 1);

    /* Shell — pass proxy socket path */
    tool_shell_register(&setup->reg, cfg->shell_timeout, cfg->workspace);

    /* Trust-level policy bundle via shared helper */
    SandboxConfig trust_policy = {0};
    sandbox_policy_from_trust(trust_level, &trust_policy);

    /* Inject proxy sock path + secrets + trust-level policy into shell config */
    ToolEntry *shell_entry = tools_lookup(&setup->reg, "shell_exec");
    if (shell_entry && shell_entry->user_data) {
        ShellConfig *sc = (ShellConfig *)shell_entry->user_data;
        sc->proxy_sock = proxy_sock_path(&setup->proxy_ctx);
        sc->secrets = setup->secrets;
        sc->secret_count = setup->secret_count;
        sc->cwd_path = getenv("CCLAW_PATH");  /* T276/V22a: CWD rw in CLI mode */
        sc->db_path = cfg->db_path;           /* mask .cclaw_key + db ciphertext from shell children */
        sc->env_file = cfg->env_file;         /* mask provider env file from shell children */
        sc->sandbox = trust_policy.sandbox;
        sc->env_mode = trust_policy.env_mode;
        sc->net_mode = trust_policy.net_mode;
        sc->mount_cwd = trust_policy.mount_cwd;
        sc->workspace_ro = trust_policy.workspace_ro;
        sc->rlimits.nproc = trust_policy.rlimits.nproc;
        sc->rlimits.as_mb = trust_policy.rlimits.as_mb;
        sc->rlimits.cpu_sec = trust_policy.rlimits.cpu_sec;
        sc->read_paths = setup->caps.read_paths;
        sc->read_path_count = setup->caps.read_count;
        sc->write_paths = setup->caps.write_paths;
        sc->write_path_count = setup->caps.write_count;
    }

    /* File tools — forked sandbox path shares trust policy with shell */
    setup->file_read_ctx.workspace = cfg->workspace;
    setup->file_read_ctx.cwd_path = getenv("CCLAW_PATH");
    setup->file_read_ctx.db_path = cfg->db_path;
    setup->file_read_ctx.read_only = trust_policy.workspace_ro;
    setup->file_read_ctx.sandbox = trust_policy.sandbox;
    setup->file_read_ctx.workspace_ro = trust_policy.workspace_ro;
    setup->file_read_ctx.mount_cwd = trust_policy.mount_cwd;
    setup->file_read_ctx.env_mode = trust_policy.env_mode;
    setup->file_read_ctx.rlimits.nproc = trust_policy.rlimits.nproc;
    setup->file_read_ctx.rlimits.as_mb = trust_policy.rlimits.as_mb;
    setup->file_read_ctx.rlimits.cpu_sec = trust_policy.rlimits.cpu_sec;
    setup->file_read_ctx.read_paths = setup->caps.read_paths;
    setup->file_read_ctx.read_path_count = setup->caps.read_count;
    setup->file_read_ctx.write_paths = setup->caps.write_paths;
    setup->file_read_ctx.write_path_count = setup->caps.write_count;
    tool_file_read_register(&setup->reg, &setup->file_read_ctx);
    tool_file_write_register(&setup->reg, &setup->file_read_ctx);
    tool_file_list_register(&setup->reg, &setup->file_read_ctx);
    tool_file_find_register(&setup->reg, &setup->file_read_ctx);
    tool_file_edit_register(&setup->reg, &setup->file_read_ctx);
    tool_file_grep_register(&setup->reg, &setup->file_read_ctx);

    /* JS eval with per-agent allowed_hosts */
    setup->js_eval_ctx.allowed_hosts = setup->caps.hosts;
    setup->js_eval_ctx.allowed_hosts_count = setup->caps.host_count;
    setup->js_eval_ctx.host_mode = (trust_level && strcmp(trust_level, "host") == 0) ? 1 : 0;
    setup->js_eval_ctx.trust_level = trust_level;
    setup->js_eval_ctx.read_paths = setup->caps.read_paths;
    setup->js_eval_ctx.read_path_count = setup->caps.read_count;
    setup->js_eval_ctx.write_paths = setup->caps.write_paths;
    setup->js_eval_ctx.write_path_count = setup->caps.write_count;
    tool_js_eval_register(&setup->reg, &setup->js_eval_ctx);

    /* V46: web_fetch policy */
    setup->web_policy.allowed_hosts = setup->caps.hosts;
    setup->web_policy.allowed_count = setup->caps.host_count;
    setup->web_policy.blocked_hosts = NULL;
    setup->web_policy.blocked_count = 0;
    setup->web_policy.block_private = 1;
    tool_web_fetch_register(&setup->reg, (setup->caps.host_count > 0) ? &setup->web_policy : NULL);

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

    /* T254/T255/T256: Load extensions from workspace/extensions/ */
    extension_ctx_init(&setup->ext_ctx, setup->js_rt);
    if (cfg->workspace) {
        size_t ext_count = 0;
        char **ext_paths = extension_discover(cfg->workspace, &ext_count);
        if (ext_paths && ext_count > 0) {
            extension_load(ext_paths, ext_count, setup->js_rt, &setup->reg, cfg,
                           &setup->ext_ctx, &setup->js_eval_ctx);
            extension_list_free(ext_paths, ext_count);
        }
    }

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

    /* Daemon-mode only tools */
    if (mode == AGENT_SETUP_DAEMON) {
        /* Approval — request_config context already registered above;
         * the resolve logic lives in main.c (resolve_approval) and uses
         * the same approval module. No extra registration needed here. */

        /* Agent launch — only register if depth allows spawning */
        setup->launch_ctx.db = db;
        setup->launch_ctx.session_id = session_id;
        int depth = session_get_depth(db, session_id);
        if (depth < agent_max_depth(db)) {
            tool_launch_agent_register(&setup->reg, &setup->launch_ctx);
        }
        tool_check_session_register(&setup->reg, &setup->launch_ctx);
    }

    tools_sync_to_db(&setup->reg, db);
    return 0;
}

void agent_setup_refresh_caps(AgentSetup *setup, sqlite3 *db, const char *agent) {
    agent_caps_refresh(db, agent, &setup->caps);

    /* Rebind all consumer pointers to the new arrays */
    setup->file_read_ctx.read_paths = setup->caps.read_paths;
    setup->file_read_ctx.read_path_count = setup->caps.read_count;
    setup->file_read_ctx.write_paths = setup->caps.write_paths;
    setup->file_read_ctx.write_path_count = setup->caps.write_count;

    setup->js_eval_ctx.allowed_hosts = setup->caps.hosts;
    setup->js_eval_ctx.allowed_hosts_count = setup->caps.host_count;
    setup->js_eval_ctx.read_paths = setup->caps.read_paths;
    setup->js_eval_ctx.read_path_count = setup->caps.read_count;
    setup->js_eval_ctx.write_paths = setup->caps.write_paths;
    setup->js_eval_ctx.write_path_count = setup->caps.write_count;

    ToolEntry *shell_entry = tools_lookup(&setup->reg, "shell_exec");
    if (shell_entry && shell_entry->user_data) {
        ShellConfig *sc = (ShellConfig *)shell_entry->user_data;
        sc->read_paths = setup->caps.read_paths;
        sc->read_path_count = setup->caps.read_count;
        sc->write_paths = setup->caps.write_paths;
        sc->write_path_count = setup->caps.write_count;
    }

    setup->web_policy.allowed_hosts = setup->caps.hosts;
    setup->web_policy.allowed_count = setup->caps.host_count;

    if (setup->js_rt)
        js_runtime_set_hosts(setup->js_rt, setup->caps.hosts, setup->caps.host_count);
}

void agent_setup_destroy(AgentSetup *setup) {
    proxy_stop(&setup->proxy_ctx);
    extension_ctx_destroy(&setup->ext_ctx);
    js_runtime_destroy(setup->js_rt);
    shell_secrets_free(setup->secrets, setup->secret_count);
    agent_caps_free(&setup->caps);
    tools_free(&setup->reg);
}
