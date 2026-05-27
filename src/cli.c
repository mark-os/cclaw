#define _POSIX_C_SOURCE 200809L
#include "cli.h"
#include "agent.h"
#include "agent_config.h"
#include "db.h"
#include "tools.h"
#include "tool_shell.h"
#include "tool_file.h"
#include "tool_js.h"
#include "tool_cron.h"
#include "tool_agent.h"
#include "tool_web_fetch.h"
#include "tool_db_query.h"
#include "tool_memory.h"
#include "tool_approval.h"
#include "tool_bootstrap.h"
#include "shutdown.h"
#include "daemon.h"
#include "context.h"
#include "secret.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* T115: display truncation limits (aggressive — shorter than V40 LLM limit) */
#define CLI_DISPLAY_MAX_BYTES 1024
#define CLI_DISPLAY_MAX_LINES 20

/* T115: progress callback for mid-turn streaming to terminal */
static void cli_progress(ProgressEvent event, const char *name,
                         const char *data, void *user_data) {
    (void)user_data;
    switch (event) {
    case PROGRESS_ASSISTANT_TEXT:
        printf("\033[2m%s\033[0m\n", data);
        break;
    case PROGRESS_TOOL_START:
        printf("\033[33m→ %s\033[0m", name);
        if (data && data[0]) {
            size_t len = strlen(data);
            if (len <= 80)
                printf("(%s)", data);
            else
                printf("(%.77s...)", data);
        }
        printf("\n");
        break;
    case PROGRESS_TOOL_RESULT:
        if (!data || !data[0]) break;
        {
            size_t len = strlen(data);
            int lines = 0;
            size_t cut = 0;
            for (size_t i = 0; i < len && i < CLI_DISPLAY_MAX_BYTES; i++) {
                if (data[i] == '\n') {
                    lines++;
                    if (lines >= CLI_DISPLAY_MAX_LINES) { cut = i; break; }
                }
            }
            if (cut == 0 && len > CLI_DISPLAY_MAX_BYTES) cut = CLI_DISPLAY_MAX_BYTES;
            if (cut > 0) {
                printf("\033[2m  %.*s\n  [... %zu bytes truncated]\033[0m\n",
                       (int)cut, data, len - cut);
            } else {
                printf("\033[2m  %s\033[0m\n", data);
            }
        }
        break;
    }
    fflush(stdout);
}

/* Tool dispatch via registry */
static char *cli_dispatch(const char *name, const char *arguments, void *user_data) {
    ToolRegistry *reg = (ToolRegistry *)user_data;
    ToolEntry *e = tools_lookup(reg, name);
    if (!e) {
        char *err = malloc(128);
        if (err) snprintf(err, 128, "error: unknown tool '%s'", name);
        return err;
    }
    return e->handler(arguments, e->user_data);
}

/* Print assistant response from the last entry in the session branch */
static void print_response(sqlite3 *db, int64_t session_id) {
    int count = 0;
    Entry *entries = session_get_branch(db, session_id, &count);
    if (!entries || count == 0) return;

    /* Walk backwards to find last assistant message with content */
    for (int i = count - 1; i >= 0; i--) {
        if (entries[i].message.role == ROLE_ASSISTANT && entries[i].message.content) {
            printf("%s\n", entries[i].message.content);
            break;
        }
    }
    entry_branch_free(entries, count);
}

/* Prompt user to select or create a session. Returns session id or -1. */
static int64_t cli_select_session(sqlite3 *db, const CliOpts *opts) {
    /* --new: force create */
    if (opts && opts->session_id == 0)
        return session_create(db, "cli", NULL, -1, 0);

    /* --session-id=N: use specific session */
    if (opts && opts->session_id > 0)
        return opts->session_id;

    /* Non-interactive: list sessions and exit */
    if (!isatty(STDIN_FILENO)) {
        int count = 0;
        Session *sessions = session_list(db, &count);
        if (!sessions || count == 0) {
            fprintf(stderr, "no sessions. use --new to create one.\n");
        } else {
            int show = count < 10 ? count : 10;
            for (int i = 0; i < show; i++) {
                fprintf(stderr, "%lld\t%s\n", (long long)sessions[i].id,
                        sessions[i].name ? sessions[i].name : "(unnamed)");
            }
            if (count > 10)
                fprintf(stderr, "... and %d more\n", count - 10);
            fprintf(stderr, "use --session-id=N or --new\n");
        }
        session_list_free(sessions, count);
        return -1;
    }

    /* Interactive: prompt */
    int count = 0;
    Session *sessions = session_list(db, &count);

    if (!sessions || count == 0) {
        session_list_free(sessions, count);
        return session_create(db, "cli", NULL, -1, 0);
    }

    printf("sessions:\n");
    for (int i = 0; i < count; i++) {
        printf("  %d) [%lld] %s\n", i + 1, (long long)sessions[i].id,
               sessions[i].name ? sessions[i].name : "(unnamed)");
    }
    printf("  n) new session\n");
    printf("select: ");
    fflush(stdout);

    char buf[32];
    if (!fgets(buf, sizeof(buf), stdin)) {
        session_list_free(sessions, count);
        return -1;
    }

    int64_t result;
    if (buf[0] == 'n' || buf[0] == 'N') {
        result = session_create(db, "cli", NULL, -1, 0);
    } else {
        int choice = atoi(buf);
        if (choice >= 1 && choice <= count) {
            result = sessions[choice - 1].id;
        } else {
            fprintf(stderr, "invalid choice\n");
            result = -1;
        }
    }

    session_list_free(sessions, count);
    return result;
}

int cli_run(const Config *cfg, const CliOpts *opts) {
    if (!cfg) return -1;
    if (!cfg->provider.api_key) {
        fprintf(stderr, "error: OPENROUTER_API_KEY not set\n");
        return -1;
    }

    sqlite3 *db = db_open(cfg->db_path);
    if (!db) {
        fprintf(stderr, "error: cannot open database '%s'\n", cfg->db_path);
        return -1;
    }

    /* V52,T172: Load/create secret key for kv encryption */
    uint8_t secret_key[32];
    if (secret_key_load_or_create(cfg->db_path, secret_key) == 0)
        db_set_secret_key(secret_key);

    /* V57: mmap + reduced cache for agent processes */
    db_set_agent_pragmas(db);

    workspace_init(cfg);

    int64_t session_id = cli_select_session(db, opts);
    if (session_id < 0) {
        db_close(db);
        return -1;
    }

    /* Mark session as running — CLI owns it */
    session_set_state(db, session_id, "running");

    /* T72: Echo unread inbox count on session resume */
    int unread = inbox_count(db, session_id);
    if (unread > 0)
        printf("[%d unread inbox message%s]\n", unread, unread == 1 ? "" : "s");

    /* T104: Load per-agent config for allowed_hosts */
    char *agent_name = session_get_agent_name(db, session_id);
    AgentConfig *ac = agent_name ? agent_config_load("agents", agent_name) : NULL;

    /* Register tools */
    ToolRegistry reg;
    tools_init(&reg);
    tool_shell_register(&reg, cfg->shell_timeout, cfg->workspace);

    /* T118: file_read allows workspace + session temp dir */
    char tmp_dir[64];
    session_tmp_dir(session_id, tmp_dir, sizeof(tmp_dir));
    FileReadCtx file_read_ctx = {.workspace = cfg->workspace,
                                  .extra_read_path = tmp_dir};
    tool_file_read_register(&reg, &file_read_ctx);
    tool_file_write_register(&reg, cfg->workspace);

    /* T104: pass per-agent allowed_hosts to js_eval */
    JsEvalCtx js_eval_ctx = {
        .allowed_hosts = ac ? ac->allowed_hosts : NULL,
        .allowed_hosts_count = ac ? ac->allowed_hosts_count : 0
    };
    tool_js_eval_register(&reg, &js_eval_ctx);
    tool_web_fetch_register(&reg, NULL);
    tool_db_query_register(&reg, db);

    /* T153: memory block tools */
    ToolMemoryCtx mem_ctx = {.db = db, .agent_name = agent_name};
    tool_memory_register(&reg, &mem_ctx);

    /* T147: approval_request tool */
    ToolApprovalCtx approval_ctx = {.db = db, .session_id = session_id,
                                    .agent_name = agent_name};
    tool_approval_register(&reg, &approval_ctx);

    /* T190/T191: bootstrap tools */
    ToolBootstrapCtx bootstrap_ctx = {.db = db};
    tool_configure_provider_register(&reg, &bootstrap_ctx);
    tool_configure_channel_register(&reg, &bootstrap_ctx);

    JsDefineCtx js_ctx = {.db = db, .session_id = session_id, .reg = &reg, .rt = NULL};
    tool_js_define_register(&reg, &js_ctx);

    ToolCronCtx cron_ctx = {.db = db, .session_id = session_id};
    tool_cron_register(&reg, &cron_ctx);

    /* Resolve self path for sub-agent spawning */
    char cli_self_path[4096];
    ssize_t cli_sp_len = readlink("/proc/self/exe", cli_self_path, sizeof(cli_self_path) - 1);
    if (cli_sp_len > 0) cli_self_path[cli_sp_len] = '\0';
    else strcpy(cli_self_path, "./build/cclaw");

    int has_daemon = daemon_is_running(cfg->db_path);
    AgentLaunchCtx sa_ctx = {.db = db, .session_id = session_id,
                             .self_path = cli_self_path,
                             .daemon_mode = has_daemon > 0};
    tool_launch_agent_register(&reg, &sa_ctx);

    /* Create persistent JS runtime and replay session tools */
    JsSessionRuntime *js_rt = js_runtime_create();
    if (js_rt && ac && ac->allowed_hosts_count > 0)
        js_runtime_set_hosts(js_rt, ac->allowed_hosts, ac->allowed_hosts_count);
    js_ctx.rt = js_rt;
    tool_js_load_session(db, session_id, &reg, js_rt);

    size_t tool_count = 0;
    const ToolSchema *schemas = tools_schemas(&reg, &tool_count);

    /* Append system message only for fresh sessions */
    int branch_count = 0;
    Entry *branch = session_get_branch(db, session_id, &branch_count);
    if (branch_count == 0) {
        char *prompt = agent_build_system_prompt(db, agent_name, session_id,
                                                "agents", cfg);
        Message sys_msg = {.role = ROLE_SYSTEM, .content = prompt};
        entry_append(db, session_id, &sys_msg);
        free(prompt);
    }
    entry_branch_free(branch, branch_count);

    /* Single-turn mode: -p <prompt> */
    if (opts->prompt) {
        Message user_msg = {.role = ROLE_USER, .content = (char *)opts->prompt};
        entry_append(db, session_id, &user_msg);

        AgentContext ctx = {0};
        ctx.db = db;
        ctx.session_id = session_id;
        ctx.cfg = cfg;
        ctx.dispatch = cli_dispatch;
        ctx.dispatch_data = &reg;
        ctx.tools = schemas;
        ctx.tool_count = tool_count;
        ctx.debug = cfg->debug;
        ctx.progress = cli_progress;

        int rc = agent_run(&ctx);
        if (rc != 0)
            fprintf(stderr, "error: agent failed\n");
        else {
            print_response(db, session_id);
            /* V58,T161: compact if branch too long */
            session_try_compact(db, session_id, cfg);
        }

        session_set_state(db, session_id, "idle");
        if (has_daemon > 0)
            daemon_signal_external(cfg->db_path, session_id);
        tools_free(&reg);
        js_runtime_destroy(js_rt);
        agent_config_free(ac);
        free(agent_name);
        db_close(db);
        return rc == 0 ? 0 : -1;
    }

    printf("cclaw cli (type 'exit' or Ctrl-D to quit)\n");

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;

    while (!shutdown_requested()) {
        printf("> ");
        fflush(stdout);

        line_len = getline(&line, &line_cap, stdin);
        if (line_len < 0) break;  /* EOF or signal */
        if (shutdown_requested()) break;

        /* Strip trailing newline */
        if (line_len > 0 && line[line_len - 1] == '\n')
            line[line_len - 1] = '\0';

        if (line[0] == '\0') continue;
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) break;

        /* Append user message */
        Message user_msg = {.role = ROLE_USER, .content = line};
        entry_append(db, session_id, &user_msg);

        /* Run agent */
        AgentContext ctx = {0};
        ctx.db = db;
        ctx.session_id = session_id;
        ctx.cfg = cfg;
        ctx.dispatch = cli_dispatch;
        ctx.dispatch_data = &reg;
        ctx.tools = schemas;
        ctx.tool_count = tool_count;
        ctx.debug = cfg->debug;
        ctx.progress = cli_progress;
        ctx.progress_data = NULL;

        int rc = agent_run(&ctx);
        if (rc != 0) {
            fprintf(stderr, "error: agent failed\n");
        } else {
            print_response(db, session_id);
            session_try_compact(db, session_id, cfg);
        }
    }

    free(line);
    session_set_state(db, session_id, "idle");
    if (has_daemon > 0)
        daemon_signal_external(cfg->db_path, session_id);
    js_runtime_destroy(js_rt);
    tools_free(&reg);
    agent_config_free(ac);
    free(agent_name);
    db_close(db);
    return 0;
}
