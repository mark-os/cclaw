#define _POSIX_C_SOURCE 200809L
#include "agent_setup.h"
#include "extension.h"
#include "tool_shell.h"
#include "tool_web_fetch.h"
#include "tool_db_query.h"
#include "tool_cron.h"
#include "tool_request_config.h"
#include "context.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int agent_setup_init(AgentSetup *setup, sqlite3 *db, int64_t session_id,
                     const Config *cfg, const char *agent_name,
                     char **allowed_hosts, size_t allowed_hosts_count,
                     int mode) {
    memset(setup, 0, sizeof(*setup));
    tools_init(&setup->reg);

    /* V83: Start credential proxy thread for shell children */
    if (cfg->workspace)
        proxy_start(&setup->proxy_ctx, cfg->workspace, allowed_hosts, allowed_hosts_count);

    /* V88: Collect secrets from env, clear from process env */
    setup->secrets = shell_secrets_collect(&setup->secret_count);

    /* Shell — pass proxy socket path */
    tool_shell_register(&setup->reg, cfg->shell_timeout, cfg->workspace);
    /* Inject proxy sock path + secrets + yolo into shell config */
    ToolEntry *shell_entry = tools_lookup(&setup->reg, "shell_exec");
    if (shell_entry && shell_entry->user_data) {
        ShellConfig *sc = (ShellConfig *)shell_entry->user_data;
        sc->proxy_sock = proxy_sock_path(&setup->proxy_ctx);
        sc->secrets = setup->secrets;
        sc->secret_count = setup->secret_count;
        sc->cwd_path = getenv("CCLAW_PATH");  /* T276/V22a: CWD rw in CLI mode */
        const char *yolo_env = getenv("CCLAW_YOLO");
        sc->yolo = (yolo_env && yolo_env[0] == '1') ? 1 : 0;
        /* T301/V22a: per-agent sandbox config */
        const char *sandbox_env = getenv("CCLAW_SANDBOX");
        sc->sandbox = (!sandbox_env || strcmp(sandbox_env, "none") != 0) ? 1 : 0;
    }

    /* File read/write — T118: allow workspace + session temp dir; T228: CCLAW_PATH */
    char tmp_dir[64];
    session_tmp_dir(session_id, tmp_dir, sizeof(tmp_dir));
    setup->file_read_ctx.workspace = cfg->workspace;
    setup->file_read_ctx.extra_read_path = tmp_dir;
    setup->file_read_ctx.cclaw_path = getenv("CCLAW_PATH");
    tool_file_read_register(&setup->reg, &setup->file_read_ctx);
    tool_file_write_register(&setup->reg, cfg->workspace);

    /* JS eval with per-agent allowed_hosts */
    setup->js_eval_ctx.allowed_hosts = allowed_hosts;
    setup->js_eval_ctx.allowed_hosts_count = allowed_hosts_count;
    tool_js_eval_register(&setup->reg, &setup->js_eval_ctx);

    /* V46: web_fetch policy */
    setup->web_policy.allowed_hosts = allowed_hosts;
    setup->web_policy.allowed_count = allowed_hosts_count;
    setup->web_policy.blocked_hosts = NULL;
    setup->web_policy.blocked_count = 0;
    setup->web_policy.block_private = 1;
    tool_web_fetch_register(&setup->reg, (allowed_hosts_count > 0) ? &setup->web_policy : NULL);

    /* db_query */
    tool_db_query_register(&setup->reg, db);

    /* Memory tools */
    setup->mem_ctx.db = db;
    setup->mem_ctx.agent_name = (char *)agent_name;
    tool_memory_register(&setup->reg, &setup->mem_ctx);

    /* JS persistent runtime + define tool */
    setup->js_rt = js_runtime_create();
    if (setup->js_rt && allowed_hosts_count > 0)
        js_runtime_set_hosts(setup->js_rt, allowed_hosts, allowed_hosts_count);
    setup->js_def_ctx.db = db;
    setup->js_def_ctx.session_id = session_id;
    setup->js_def_ctx.reg = &setup->reg;
    setup->js_def_ctx.rt = setup->js_rt;
    tool_js_define_register(&setup->reg, &setup->js_def_ctx);
    tool_js_load_session(db, session_id, &setup->reg, setup->js_rt);

    /* T254/T255/T256: Load extensions from workspace/extensions/ */
    extension_ctx_init(&setup->ext_ctx, setup->js_rt);
    if (cfg->workspace) {
        size_t ext_count = 0;
        char **ext_paths = extension_discover(cfg->workspace, &ext_count);
        if (ext_paths && ext_count > 0) {
            extension_load(ext_paths, ext_count, setup->js_rt, &setup->reg, cfg,
                           &setup->ext_ctx);
            extension_list_free(ext_paths, ext_count);
        }
    }

    /* V116: Set tool registry on JS runtime for callTool dispatch */
    if (setup->js_rt)
        js_runtime_set_registry(setup->js_rt, &setup->reg);

    /* T274/V120: request_config — CLI inline tool/host grant */
    tool_request_config_register(&setup->reg);

    /* Daemon-mode only tools */
    if (mode == AGENT_SETUP_DAEMON) {
        /* Approval */
        setup->approval_ctx.db = db;
        setup->approval_ctx.session_id = session_id;
        setup->approval_ctx.agent_name = (char *)agent_name;
        tool_approval_register(&setup->reg, &setup->approval_ctx);

        /* Bootstrap tools */
        setup->bootstrap_ctx.db = db;
        setup->bootstrap_ctx.session_id = session_id;
        setup->bootstrap_ctx.agent_name = (char *)agent_name;
        tool_configure_provider_register(&setup->reg, &setup->bootstrap_ctx);
        tool_configure_channel_register(&setup->reg, &setup->bootstrap_ctx);
        tool_create_agent_register(&setup->reg, &setup->bootstrap_ctx);

        /* Agent launch */
        setup->launch_ctx.db = db;
        setup->launch_ctx.session_id = session_id;
        setup->launch_ctx.daemon_mode = 1;
        tool_launch_agent_register(&setup->reg, &setup->launch_ctx);
        tool_check_agent_register(&setup->reg, &setup->launch_ctx);
    }

    return 0;
}

const ToolSchema *agent_setup_schemas(AgentSetup *setup, size_t *count) {
    return tools_schemas(&setup->reg, count);
}

void agent_setup_destroy(AgentSetup *setup) {
    proxy_stop(&setup->proxy_ctx);
    extension_ctx_destroy(&setup->ext_ctx);
    js_runtime_destroy(setup->js_rt);
    shell_secrets_free(setup->secrets, setup->secret_count);
    tools_free(&setup->reg);
}
