#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include "cclaw.h"
#include "config.h"
#include "cli.h"
#include "telegram.h"
#include "web.h"
#include "heartbeat.h"
#include "cron.h"
#include "db.h"
#include "agent.h"
#include "tools.h"
#include "tool_shell.h"
#include "tool_file.h"
#include "tool_js.h"
#include "tool_cron.h"
#include "tool_subagent.h"
#include "tool_web_fetch.h"
#include "shutdown.h"

/* Tool dispatch via registry (shared with sub-agent mode) */
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

/* Run as sub-agent: execute task, store result in DB, exit */
static int run_sub_agent(const Config *cfg, int64_t session_id, const char *task, int depth) {
    sqlite3 *db = db_open(cfg->db_path);
    if (!db) return 1;

    /* Append system + user messages */
    Message sys_msg = {.role = ROLE_SYSTEM,
        .content = "You are a CClaw sub-agent. Complete the assigned task concisely."};
    entry_append(db, session_id, &sys_msg);

    Message user_msg = {.role = ROLE_USER, .content = (char *)task};
    entry_append(db, session_id, &user_msg);

    /* Register tools */
    ToolRegistry reg;
    tools_init(&reg);
    tool_shell_register(&reg, cfg->shell_timeout);
    tool_file_read_register(&reg, cfg->workspace);
    tool_file_write_register(&reg, cfg->workspace);
    tool_js_eval_register(&reg);
    tool_web_fetch_register(&reg);

    /* Sub-agents can spawn further sub-agents (up to V3 depth limit) */
    char self_path[4096];
    ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (len > 0) self_path[len] = '\0';
    else strcpy(self_path, "./build/cclaw");

    SubAgentCtx sa_ctx = {.db = db, .session_id = session_id,
                          .depth = depth, .self_path = self_path};
    tool_spawn_agent_register(&reg, &sa_ctx);
    tool_check_agent_register(&reg, &sa_ctx);

    size_t tool_count = 0;
    const ToolSchema *schemas = tools_schemas(&reg, &tool_count);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = session_id;
    ctx.cfg = cfg;
    ctx.dispatch = dispatch_tools;
    ctx.dispatch_data = &reg;
    ctx.tools = schemas;
    ctx.tool_count = tool_count;
    ctx.debug = cfg->debug;

    int rc = agent_run(&ctx);

    /* Store final assistant response as sub-agent result */
    int count = 0;
    Entry *entries = session_get_branch(db, session_id, &count);
    const char *result = NULL;
    if (entries) {
        for (int i = count - 1; i >= 0; i--) {
            if (entries[i].message.role == ROLE_ASSISTANT && entries[i].message.content) {
                result = entries[i].message.content;
                break;
            }
        }
    }

    /* Find our sub_agent row and mark finished */
    const char *sql = "SELECT id FROM sub_agents WHERE session_id=? AND status='running';";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, session_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int64_t agent_id = sqlite3_column_int64(stmt, 0);
            subagent_finish(db, agent_id, rc == 0 ? "done" : "error",
                           result ? result : "no response");
        }
        sqlite3_finalize(stmt);
    }

    entry_branch_free(entries, count);
    tools_free(&reg);
    db_close(db);
    return rc == 0 ? 0 : 1;
}

int main(int argc, char *argv[]) {
    int cli_mode = 0;
    int debug_mode = 0;
    int sub_agent_mode = 0;
    int64_t sa_session_id = -1;
    const char *sa_task = NULL;
    int sa_depth = 1;
    const char *config_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--cli") == 0) {
            cli_mode = 1;
        } else if (strcmp(argv[i], "--debug") == 0) {
            debug_mode = 1;
        } else if (strcmp(argv[i], "--sub-agent") == 0) {
            sub_agent_mode = 1;
        } else if (strncmp(argv[i], "--session-id=", 13) == 0) {
            sa_session_id = atoll(argv[i] + 13);
        } else if (strncmp(argv[i], "--task=", 7) == 0) {
            sa_task = argv[i] + 7;
        } else if (strncmp(argv[i], "--depth=", 8) == 0) {
            sa_depth = atoi(argv[i] + 8);
        } else if (argv[i][0] != '-') {
            config_path = argv[i];
        }
    }

    Config *cfg = config_load(config_path);
    if (!cfg) {
        fprintf(stderr, "error: failed to load config\n");
        return 1;
    }
    if (debug_mode) cfg->debug = 1;

    shutdown_init();

    if (sub_agent_mode) {
        if (sa_session_id < 0 || !sa_task) {
            fprintf(stderr, "error: --sub-agent requires --session-id=X --task=\"...\"\n");
            config_free(cfg);
            return 1;
        }
        int rc = run_sub_agent(cfg, sa_session_id, sa_task, sa_depth);
        config_free(cfg);
        return rc;
    }

    if (cli_mode) {
        int rc = cli_run(cfg);
        config_free(cfg);
        return rc == 0 ? 0 : 1;
    }

    /* Daemon mode: start Telegram poller */
    if (!cfg->telegram_token || cfg->telegram_token[0] == '\0') {
        fprintf(stderr, "error: CCLAW_TELEGRAM_TOKEN not set (required for daemon mode)\n");
        config_free(cfg);
        return 1;
    }

    sqlite3 *db = db_open(cfg->db_path);
    if (!db) {
        fprintf(stderr, "error: cannot open database '%s'\n", cfg->db_path);
        config_free(cfg);
        return 1;
    }

    printf("cclaw %s — daemon mode\n", CCLAW_VERSION);

    if (telegram_start(cfg, db) != 0) {
        fprintf(stderr, "error: failed to start telegram poller\n");
        db_close(db);
        config_free(cfg);
        return 1;
    }
    printf("telegram poller started\n");

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

    while (!shutdown_requested()) {
        sleep(1);
        subagent_reap(db);
    }

    printf("\nshutting down...\n");
    cron_stop();
    heartbeat_stop();
    web_stop();
    telegram_stop();
    db_close(db);
    config_free(cfg);
    return 0;
}
