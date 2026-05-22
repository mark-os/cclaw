#define _POSIX_C_SOURCE 200809L
#include "cli.h"
#include "agent.h"
#include "db.h"
#include "tools.h"
#include "tool_shell.h"
#include "tool_file.h"
#include "tool_js.h"
#include "tool_cron.h"
#include "tool_subagent.h"
#include "tool_web_fetch.h"
#include "tool_db_query.h"
#include "shutdown.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
static int64_t cli_select_session(sqlite3 *db) {
    int count = 0;
    Session *sessions = session_list(db, &count);

    if (!sessions || count == 0) {
        session_list_free(sessions, count);
        return session_create(db, "cli");
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
        result = session_create(db, "cli");
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

int cli_run(const Config *cfg) {
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

    int64_t session_id = cli_select_session(db);
    if (session_id < 0) {
        fprintf(stderr, "error: cannot select session\n");
        db_close(db);
        return -1;
    }

    /* Register tools */
    ToolRegistry reg;
    tools_init(&reg);
    tool_shell_register(&reg, cfg->shell_timeout);
    tool_file_read_register(&reg, cfg->workspace);
    tool_file_write_register(&reg, cfg->workspace);
    tool_js_eval_register(&reg);
    tool_web_fetch_register(&reg);
    tool_db_query_register(&reg, db);

    JsDefineCtx js_ctx = {.db = db, .session_id = session_id, .reg = &reg, .rt = NULL};
    tool_js_define_register(&reg, &js_ctx);

    ToolCronCtx cron_ctx = {.db = db, .session_id = session_id};
    tool_cron_register(&reg, &cron_ctx);

    /* Resolve self path for sub-agent spawning */
    char self_path[4096];
    ssize_t self_len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (self_len > 0) self_path[self_len] = '\0';
    else strcpy(self_path, "./build/cclaw");

    SubAgentCtx sa_ctx = {.db = db, .session_id = session_id,
                          .depth = 0, .self_path = self_path};
    tool_spawn_agent_register(&reg, &sa_ctx);

    /* Create persistent JS runtime and replay session tools */
    JsSessionRuntime *js_rt = js_runtime_create();
    js_ctx.rt = js_rt;
    tool_js_load_session(db, session_id, &reg, js_rt);

    size_t tool_count = 0;
    const ToolSchema *schemas = tools_schemas(&reg, &tool_count);

    /* Append system message only for fresh sessions */
    int branch_count = 0;
    Entry *branch = session_get_branch(db, session_id, &branch_count);
    if (branch_count == 0) {
        char *prompt = config_render_system_prompt(cfg, session_id);
        Message sys_msg = {.role = ROLE_SYSTEM, .content = prompt};
        entry_append(db, session_id, &sys_msg);
        free(prompt);
    }
    entry_branch_free(branch, branch_count);

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

        int rc = agent_run(&ctx);
        if (rc != 0) {
            fprintf(stderr, "error: agent failed\n");
        } else {
            print_response(db, session_id);
        }
    }

    free(line);
    js_runtime_destroy(js_rt);
    tools_free(&reg);
    db_close(db);
    return 0;
}
