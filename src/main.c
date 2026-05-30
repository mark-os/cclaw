#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <libgen.h>
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
#include "agent_setup.h"
#include "agent_exit.h"
#include "shutdown.h"
#include "context.h"
#include "daemon.h"
#include "secret.h"

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
 * Daemon fork+exec target. V73/V74: opens only own agent.db, config from env. */
static int run_agent_turn(int64_t session_id) {
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

    /* V74,T198: Config purely from env vars — no DB reads for config */
    Config *cfg = config_load_from_env();
    if (!cfg) return 1;

    /* V73,T198: Open agent's own DB only */
    const char *agent_db_path = getenv("CCLAW_AGENT_DB");
    if (!agent_db_path || !agent_db_path[0]) {
        /* Fallback for direct invocation (testing/CLI-spawned sub-agents) */
        agent_db_path = cfg->db_path;
    }
    sqlite3 *db = db_open_agent(agent_db_path);
    if (!db) { config_free(cfg); return 1; }

    /* V57: mmap + reduced cache for agent processes */
    db_set_agent_pragmas(db);

    /* V74: agent_name from env (daemon injects at fork) */
    const char *agent_name_env = getenv("CCLAW_AGENT_NAME");
    char *agent_name = agent_name_env ? strdup(agent_name_env) : session_get_agent_name(db, session_id);

    /* V74: Parse allowed_hosts from env (comma-separated) */
    char **allowed_hosts = NULL;
    size_t allowed_hosts_count = 0;
    {
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

    /* T206: Use shared agent_setup for tool registration (daemon mode) */
    AgentSetup setup;
    agent_setup_init(&setup, db, session_id, cfg, agent_name,
                     allowed_hosts, allowed_hosts_count, AGENT_SETUP_DAEMON);

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
    ctx.debug = cfg->debug;

    rc = agent_run(&ctx);

    /* V91: trigger compaction if enabled and session exceeds threshold */
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
    /* T200/V73: Agent sets own state in its DB before exit */
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

/* Resolve DB path: env override or default next to binary */
static char *resolve_db_path(void) {
    const char *env = getenv("CCLAW_DB_PATH");
    if (env) return strdup(env);
    char exe[4096];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len > 0) {
        exe[len] = '\0';
        char *dir = dirname(exe);
        size_t dlen = strlen(dir);
        char *p = malloc(dlen + sizeof("/cclaw.db"));
        if (p) { sprintf(p, "%s/cclaw.db", dir); return p; }
    }
    return strdup("cclaw.db");
}

static void print_usage(void) {
    printf("usage: cclaw [options]\n"
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
        } else {
            fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
            print_usage();
            return 1;
        }
    }

    /* Agent mode: self-contained, opens DB and loads config from env */
    if (agent_mode) {
        if (sa_session_id < 0) {
            fprintf(stderr, "error: --agent requires --session-id=N\n");
            return 1;
        }
        return run_agent_turn(sa_session_id);
    }

    shutdown_init();

    /* T206/V81: CLI mode — config from env vars, no cclaw.db needed */
    if (!daemon_mode) {
        Config *cfg = config_load_from_env();
        if (!cfg) {
            fprintf(stderr, "error: failed to load config\n");
            return 1;
        }
        config_load_json_fallback(cfg);
        if (debug_mode) cfg->debug = 1;

        CliOpts opts = {0};
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

    /* Daemon mode: needs cclaw.db for kv config, channels, etc. */
    char *db_path = resolve_db_path();
    sqlite3 *db = db_open_cclaw(db_path);
    if (!db) {
        fprintf(stderr, "error: cannot open database '%s'\n", db_path);
        free(db_path);
        return 1;
    }

    /* V52,T172: Load/create secret key for kv encryption */
    {
        uint8_t secret_key[32];
        if (secret_key_load_or_create(db_path, secret_key) == 0)
            db_set_secret_key(secret_key);
    }
    free(db_path);

    Config *cfg = config_load_from_kv(db);
    if (!cfg) {
        fprintf(stderr, "error: failed to load config from database\n");
        db_close(db);
        return 1;
    }
    config_load_json_fallback(cfg);
    if (debug_mode) cfg->debug = 1;

    /* Daemon mode: epoll loop, fork agents on inbox signal, reap on exit */
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

    /* T189/V68: Bootstrap — spawn ephemeral if no named agents */
    int64_t bootstrap_sid = daemon_bootstrap(db);
    if (bootstrap_sid > 0) {
        printf("bootstrap agent created (session %lld)\n", (long long)bootstrap_sid);
    }

    /* T81: daemon main loop — blocks until shutdown */
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
