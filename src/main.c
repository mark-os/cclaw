#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include "cclaw.h"
#include "config.h"
#include "cli.h"
#include "telegram.h"
#include "web.h"
#include "heartbeat.h"
#include "cron.h"
#include "db.h"
#include "agent.h"
#include "agent_config.h"
#include "landlock.h"
#include "tools.h"
#include "tool_shell.h"
#include "tool_file.h"
#include "tool_js.h"
#include "tool_cron.h"
#include "tool_agent.h"
#include "tool_web_fetch.h"
#include "tool_db_query.h"
#include "tool_soul.h"
#include "tool_memory.h"
#include "shutdown.h"
#include "daemon.h"
#include "context.h"

/* Tool dispatch via registry */
static char *dispatch_tools(const char *name, const char *arguments, void *user_data) {
    ToolRegistry *reg = (ToolRegistry *)user_data;
    ToolEntry *e = tools_lookup(reg, name);
    if (!e) {
        char *err = malloc(128);
        if (err) snprintf(err, 128, "error: unknown tool '%s'", name);
        return err;
    }
    return e->handler(arguments, e->user_data);
}

/* --agent mode: run one turn on a session, then exit.
 * Daemon fork+exec target. Loads agent config from session's agent_name. */
static int run_agent_turn(const Config *cfg, int64_t session_id) {
    /* V34: die if parent dies */
    prctl(PR_SET_PDEATHSIG, SIGTERM);

    /* V23: resource limits */
    struct rlimit rl;
    rl.rlim_cur = 256 * 1024 * 1024;
    rl.rlim_max = 256 * 1024 * 1024;
    setrlimit(RLIMIT_AS, &rl);
    rl.rlim_cur = 300;
    rl.rlim_max = 300;
    setrlimit(RLIMIT_CPU, &rl);
    rl.rlim_cur = 64;
    rl.rlim_max = 64;
    setrlimit(RLIMIT_NOFILE, &rl);

    shutdown_init();

    sqlite3 *db = db_open(cfg->db_path);
    if (!db) return 1;

    /* V20: Load per-agent config, merge with global */
    char *agent_name = session_get_agent_name(db, session_id);
    AgentConfig *ac = agent_name ? agent_config_load("agents", agent_name) : NULL;
    const Config *effective_cfg = cfg;
    Config *merged_cfg = NULL;
    if (ac) {
        merged_cfg = agent_config_merge(cfg, ac);
        if (merged_cfg) effective_cfg = merged_cfg;
    }

    /* V22: Landlock — restrict filesystem after config loaded */
    if (landlock_apply(effective_cfg->workspace, cfg->db_path, session_id) < 0) {
        fprintf(stderr, "[agent %lld] landlock unavailable, continuing without\n",
                (long long)session_id);
    }

    /* V27: Update last_route from newest inbox source before consuming */
    int peek_count = 0;
    InboxItem *items = inbox_peek(db, session_id, 100, &peek_count);
    if (items && peek_count > 0) {
        session_set_last_route(db, session_id, items[peek_count - 1].source);
        inbox_items_free(items, peek_count);
    }

    /* V18: Drain inbox into session entries */
    inbox_consume_into_entries(db, session_id, 100);

    /* Read branch — check if session is already complete */
    int branch_count = 0;
    Entry *branch = session_get_branch(db, session_id, &branch_count);
    if (branch_count > 0) {
        Entry *last = &branch[branch_count - 1];
        if (last->message.role == ROLE_ASSISTANT &&
            (last->message.stop_reason == STOP_REASON_STOP ||
             last->message.stop_reason == STOP_REASON_ERROR ||
             last->message.stop_reason == STOP_REASON_ABORTED)) {
            entry_branch_free(branch, branch_count);
            agent_config_free(ac);
            if (merged_cfg) config_free(merged_cfg);
            free(agent_name);
            db_close(db);
            return 0;
        }
    }
    if (branch_count == 0) {
        char *prompt = config_render_system_prompt(effective_cfg, session_id);
        Message sys_msg = {.role = ROLE_SYSTEM, .content = prompt};
        entry_append(db, session_id, &sys_msg);
        free(prompt);
    }
    entry_branch_free(branch, branch_count);

    /* Register tools */
    ToolRegistry reg;
    tools_init(&reg);
    tool_shell_register(&reg, effective_cfg->shell_timeout,
                        effective_cfg->workspace, ac ? ac->shell_network : 0);

    /* T118: file_read allows workspace + session temp dir */
    char tmp_dir[64];
    session_tmp_dir(session_id, tmp_dir, sizeof(tmp_dir));
    FileReadCtx file_read_ctx = {.workspace = effective_cfg->workspace,
                                  .extra_read_path = tmp_dir};
    tool_file_read_register(&reg, &file_read_ctx);
    tool_file_write_register(&reg, effective_cfg->workspace);

    /* T104: JS eval with per-agent allowed_hosts */
    JsEvalCtx js_eval_ctx = {
        .allowed_hosts = ac ? ac->allowed_hosts : NULL,
        .allowed_hosts_count = ac ? ac->allowed_hosts_count : 0
    };
    tool_js_eval_register(&reg, &js_eval_ctx);
    tool_web_fetch_register(&reg, NULL);
    tool_db_query_register(&reg, db);

    /* T120: soul_edit tool */
    ToolSoulCtx soul_ctx = {.db = db, .agent_name = agent_name};
    tool_soul_register(&reg, &soul_ctx);

    /* T121: memory_set tool */
    ToolMemoryCtx mem_ctx = {.db = db, .agent_name = agent_name};
    tool_memory_register(&reg, &mem_ctx);

    /* JS persistent runtime + define tool */
    JsSessionRuntime *js_rt = js_runtime_create();
    if (js_rt && ac && ac->allowed_hosts_count > 0)
        js_runtime_set_hosts(js_rt, ac->allowed_hosts, ac->allowed_hosts_count);
    JsDefineCtx js_def_ctx = {.db = db, .session_id = session_id,
                               .reg = &reg, .rt = js_rt};
    tool_js_define_register(&reg, &js_def_ctx);
    tool_js_load_session(db, session_id, &reg, js_rt);

    /* Cron tool */
    ToolCronCtx cron_ctx = {.db = db, .session_id = session_id};
    tool_cron_register(&reg, &cron_ctx);

    /* Agent launch tool */
    AgentLaunchCtx la_ctx = {.db = db, .session_id = session_id,
                             .daemon_mode = 1};
    tool_launch_agent_register(&reg, &la_ctx);
    tool_check_agent_register(&reg, &la_ctx);

    size_t tool_count = 0;
    const ToolSchema *schemas = tools_schemas(&reg, &tool_count);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = session_id;
    ctx.cfg = effective_cfg;
    ctx.dispatch = dispatch_tools;
    ctx.dispatch_data = &reg;
    ctx.tools = schemas;
    ctx.tool_count = tool_count;
    ctx.debug = effective_cfg->debug;

    int rc = agent_run(&ctx);

    /* Notify parent inbox if this is a child session */
    const char *sql = "SELECT parent_session_id FROM sessions WHERE id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, session_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int64_t parent_sid = sqlite3_column_int64(stmt, 0);
            if (parent_sid > 0) {
                char payload[256];
                snprintf(payload, sizeof(payload),
                         "{\"event\":\"agent_done\",\"session_id\":%lld,\"status\":\"%s\"}",
                         (long long)session_id, rc == 0 ? "done" : "error");
                inbox_insert(db, parent_sid, "agent", payload);
            }
        }
        sqlite3_finalize(stmt);
    }

    tools_free(&reg);
    js_runtime_destroy(js_rt);
    agent_config_free(ac);
    if (merged_cfg) config_free(merged_cfg);
    free(agent_name);
    db_close(db);
    return rc == 0 ? 0 : 1;
}

static void print_usage(void) {
    printf("usage: cclaw [options] [config.json]\n"
           "\n"
           "modes (default: --cli):\n"
           "  --cli              interactive CLI (stdin/stdout)\n"
           "  --daemon           run as daemon (telegram, web, cron)\n"
           "  --agent            run one agent turn (internal, used by daemon)\n"
           "\n"
           "options:\n"
           "  -p <prompt>        single-turn: send prompt, print response, exit\n"
           "  -s <id>            session id (short for --session-id=N)\n"
           "  --new              create a new session\n"
           "  --debug            enable debug output\n"
           "  --help             show this help\n");
}

int main(int argc, char *argv[]) {
    int daemon_mode = 0;
    int agent_mode = 0;
    int debug_mode = 0;
    int new_session = 0;
    int64_t sa_session_id = -1;
    const char *config_path = NULL;
    const char *prompt = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
        } else if (strcmp(argv[i], "--cli") == 0) {
            /* accepted for explicitness, cli is the default */
        } else if (strcmp(argv[i], "--daemon") == 0) {
            daemon_mode = 1;
        } else if (strcmp(argv[i], "--agent") == 0) {
            agent_mode = 1;
        } else if (strcmp(argv[i], "--debug") == 0) {
            debug_mode = 1;
        } else if (strcmp(argv[i], "--new") == 0) {
            new_session = 1;
        } else if (strcmp(argv[i], "-p") == 0) {
            if (++i >= argc) { fprintf(stderr, "error: -p requires an argument\n"); return 1; }
            prompt = argv[i];
        } else if (strcmp(argv[i], "-s") == 0) {
            if (++i >= argc) { fprintf(stderr, "error: -s requires an argument\n"); return 1; }
            sa_session_id = atoll(argv[i]);
        } else if (strncmp(argv[i], "--session-id=", 13) == 0) {
            sa_session_id = atoll(argv[i] + 13);
        } else if (argv[i][0] != '-') {
            config_path = argv[i];
        } else {
            fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
            print_usage();
            return 1;
        }
    }

    Config *cfg = config_load(config_path);
    if (!cfg) {
        fprintf(stderr, "error: failed to load config\n");
        return 1;
    }
    if (debug_mode) cfg->debug = 1;

    shutdown_init();

    if (!daemon_mode && !agent_mode) {
        CliOpts opts = {0};
        opts.config_path = config_path;
        opts.prompt = prompt;
        if (new_session)
            opts.session_id = 0;
        else if (sa_session_id > 0)
            opts.session_id = sa_session_id;
        else
            opts.session_id = -1;
        int rc = cli_run(cfg, &opts);
        config_free(cfg);
        return rc == 0 ? 0 : 1;
    }

    if (agent_mode) {
        if (sa_session_id < 0) {
            fprintf(stderr, "error: --agent requires --session-id=N\n");
            config_free(cfg);
            return 1;
        }
        int rc = run_agent_turn(cfg, sa_session_id);
        config_free(cfg);
        return rc;
    }

    /* Daemon mode: epoll loop, fork agents on inbox signal, reap on exit */
    sqlite3 *db = db_open(cfg->db_path);
    if (!db) {
        fprintf(stderr, "error: cannot open database '%s'\n", cfg->db_path);
        config_free(cfg);
        return 1;
    }

    workspace_init(cfg);

    printf("cclaw %s — daemon mode\n", CCLAW_VERSION);

    if (cfg->telegram_token && cfg->telegram_token[0] != '\0') {
        if (telegram_start(cfg, db) != 0) {
            fprintf(stderr, "warning: failed to start telegram poller\n");
        } else {
            printf("telegram poller started\n");
        }
    }

    if (web_start(cfg, db) != 0) {
        fprintf(stderr, "warning: failed to start web server on port %d\n", cfg->web_port);
    } else {
        printf("web server started on port %d\n", cfg->web_port);
    }

    if (heartbeat_start(cfg, db) != 0) {
        fprintf(stderr, "warning: failed to start heartbeat timer\n");
    } else if (cfg->heartbeat_interval > 0) {
        printf("heartbeat started (%ds interval)\n", cfg->heartbeat_interval);
    }

    if (cron_start(cfg, db) != 0) {
        fprintf(stderr, "warning: failed to start cron scheduler\n");
    } else {
        printf("cron scheduler started\n");
    }

    /* T81: daemon main loop — blocks until shutdown */
    if (config_path) daemon_set_config_path(config_path);
    daemon_run(cfg, db);

    printf("\nshutting down...\n");
    cron_stop();
    heartbeat_stop();
    web_stop();
    telegram_stop();
    db_close(db);
    config_free(cfg);
    return 0;
}
