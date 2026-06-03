#define _POSIX_C_SOURCE 200809L
#include "agent_turn.h"
#include "agent.h"
#include "agent_config.h"
#include "agent_exit.h"
#include "agent_setup.h"
#include "config.h"
#include "context.h"
#include "daemon.h"
#include "db.h"
#include "shutdown.h"
#include "tools.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>

/* V119/V124: default tool whitelist from agent_config.h */
static const char *DEFAULT_TOOLS[] = { AGENT_DEFAULT_TOOLS };
static const size_t DEFAULT_TOOLS_COUNT = AGENT_DEFAULT_TOOLS_COUNT;

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

/* T115: CLI progress callback — writes tool activity to stdout */
static void cli_progress(ProgressEvent event, const char *name,
                         const char *data, void *user_data) {
    (void)user_data;
    switch (event) {
    case PROGRESS_TOOL_START:
        fprintf(stdout, "\n\033[2m[tool: %s]\033[0m ", name ? name : "?");
        fflush(stdout);
        break;
    case PROGRESS_TOOL_RESULT: {
        size_t len = data ? strlen(data) : 0;
        if (len <= 80)
            fprintf(stdout, "\033[2m→ %s\033[0m\n", data ? data : "(empty)");
        else
            fprintf(stdout, "\033[2m→ %.77s...\033[0m\n", data);
        fflush(stdout);
        break;
    }
    case PROGRESS_ASSISTANT_TEXT:
        break;
    }
}

int agent_turn_run(int64_t session_id) {
    /* V34: die if parent dies */
    prctl(PR_SET_PDEATHSIG, SIGTERM);

    /* V23: resource limits */
    struct rlimit rl;
#ifndef __ANDROID__
    rl.rlim_cur = 256 * 1024 * 1024;
    rl.rlim_max = 256 * 1024 * 1024;
    setrlimit(RLIMIT_AS, &rl);
#endif
    rl.rlim_cur = 300;
    rl.rlim_max = 300;
    setrlimit(RLIMIT_CPU, &rl);
    rl.rlim_cur = 64;
    rl.rlim_max = 64;
    setrlimit(RLIMIT_NOFILE, &rl);

    shutdown_init();

    /* V74,T198: Config purely from env vars */
    Config *cfg = config_load_from_env();
    if (!cfg) return 1;

    /* V73,T198: Open agent's own DB only */
    const char *agent_db_path = getenv("CCLAW_AGENT_DB");
    if (!agent_db_path || !agent_db_path[0])
        agent_db_path = cfg->db_path;

    sqlite3 *db = db_open_agent(agent_db_path);
    if (!db) { config_free(cfg); return 1; }

    /* V57: mmap + reduced cache for agent processes */
    db_set_agent_pragmas(db);

    /* V74: agent_name from env (daemon injects at fork) */
    const char *agent_name_env = getenv("CCLAW_AGENT_NAME");
    char *agent_name = agent_name_env ? strdup(agent_name_env)
                                      : session_get_agent_name(db, session_id);

    /* V74: Parse allowed_hosts from env (comma-separated) — skip in yolo mode */
    char **allowed_hosts = NULL;
    size_t allowed_hosts_count = 0;
    const char *yolo_env = getenv("CCLAW_YOLO");
    int is_yolo = (yolo_env && yolo_env[0] == '1');
    if (!is_yolo) {
        const char *hosts_env = getenv("CCLAW_ALLOWED_HOSTS");
        if (hosts_env && hosts_env[0]) {
            size_t cap = 1;
            for (const char *p = hosts_env; *p; p++) if (*p == ',') cap++;
            allowed_hosts = malloc(cap * sizeof(char *));
            if (allowed_hosts) {
                char *dup = strdup(hosts_env);
                char *tok = strtok(dup, ",");
                while (tok) {
                    while (*tok == ' ') tok++;
                    if (*tok) allowed_hosts[allowed_hosts_count++] = strdup(tok);
                    tok = strtok(NULL, ",");
                }
                free(dup);
            }
        }
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

    int rc = AGENT_EXIT_DONE;

    /* Check if session is already complete */
    int branch_count = 0;
    Entry *branch = session_get_branch(db, session_id, &branch_count);
    if (branch_count > 0) {
        Entry *last = &branch[branch_count - 1];
        if (last->message.role == ROLE_ASSISTANT &&
            (last->message.stop_reason == STOP_REASON_STOP ||
             last->message.stop_reason == STOP_REASON_ERROR ||
             last->message.stop_reason == STOP_REASON_ABORTED)) {
            entry_branch_free(branch, branch_count);
            goto cleanup;
        }
    }
    if (branch_count == 0) {
        char *prompt = agent_build_system_prompt(db, agent_name, session_id,
                                                "agents", cfg);
        Message sys_msg = {.role = ROLE_SYSTEM, .content = prompt};
        entry_append(db, session_id, &sys_msg);
        free(prompt);
    }
    entry_branch_free(branch, branch_count);

    /* T206: Tool registration (mode from env) */
    const char *mode_env = getenv("CCLAW_MODE");
    int setup_mode = (mode_env && strcmp(mode_env, "cli") == 0)
                     ? AGENT_SETUP_CLI : AGENT_SETUP_DAEMON;
    AgentSetup setup;
    agent_setup_init(&setup, db, session_id, cfg, agent_name,
                     allowed_hosts, allowed_hosts_count, setup_mode);

    size_t tool_count = 0;
    const char *tools_env = getenv("CCLAW_TOOLS");
    const char **tool_whitelist = NULL;
    size_t tool_wl_count = 0;
    char *tools_dup = NULL;
    if (tools_env && tools_env[0]) {
        tools_dup = strdup(tools_env);
        size_t cap = 1;
        for (const char *p = tools_env; *p; p++) if (*p == ',') cap++;
        tool_whitelist = malloc(cap * sizeof(char *));
        if (tool_whitelist && tools_dup) {
            char *tok = strtok(tools_dup, ",");
            while (tok) {
                while (*tok == ' ') tok++;
                if (*tok) tool_whitelist[tool_wl_count++] = tok;
                tok = strtok(NULL, ",");
            }
        }
    } else if (!is_yolo) {
        /* V119/V124: absent CCLAW_TOOLS → use default set (not all tools) */
        tool_whitelist = DEFAULT_TOOLS;
        tool_wl_count = DEFAULT_TOOLS_COUNT;
    }
    const ToolSchema *schemas = tools_schemas_filtered(&setup.reg, tool_whitelist,
                                                       tool_wl_count, &tool_count);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = session_id;
    ctx.cfg = cfg;
    ctx.dispatch = dispatch_tools;
    ctx.dispatch_data = &setup.reg;
    ctx.tools = schemas;
    ctx.tool_count = tool_count;
    ctx.ext_ctx = &setup.ext_ctx;
    if (setup_mode == AGENT_SETUP_CLI)
        ctx.progress = cli_progress;

    rc = agent_run(&ctx);

    /* V91: trigger compaction if enabled */
    if (rc == 0 && cfg->compaction && session_needs_compaction(db, session_id, cfg))
        session_try_compact(db, session_id, cfg);

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

    agent_setup_destroy(&setup);
    free((void *)tool_whitelist);
    free(tools_dup);

cleanup:
    /* T200/V73: Agent sets own state before exit */
    if (rc == AGENT_EXIT_SPAWN || rc == AGENT_EXIT_APPROVAL || rc == AGENT_EXIT_CONFIG)
        session_set_state(db, session_id, "waiting");
    else
        session_set_state(db, session_id, "idle");

    for (size_t i = 0; i < allowed_hosts_count; i++) free(allowed_hosts[i]);
    free(allowed_hosts);
    free(agent_name);
    config_free(cfg);
    db_close(db);

    /* V72/T197: propagate exit codes 0-4 to process exit */
    if (rc >= AGENT_EXIT_DONE && rc <= AGENT_EXIT_CONFIG)
        return rc;
    return AGENT_EXIT_ERROR;
}
