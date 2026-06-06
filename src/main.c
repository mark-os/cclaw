#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <limits.h>
#include <libgen.h>
#include "cclaw.h"
#include "config.h"
#include "log.h"
#include "agent_config.h"
#include "agent_exit.h"
#include "turn_loop.h"
#include "llm_child.h"
#include "web.h"
#include "heartbeat.h"
#include "cron.h"
#include "db.h"
#include "shutdown.h"
#include "context.h"
#include "daemon.h"
#include "secret.h"

/* Track child pid so SIGINT can forward to it */

/* Print last assistant response from session branch */
static void print_response(sqlite3 *db, int64_t session_id) {
    char *text = get_response_text(db, session_id);
    if (text) {
        printf("%s\n", text);
        free(text);
    }
}

/* T272/V118: Session picker — show (id, first_prompt[:50], last_prompt[:50], created_at) */
static int64_t cli_select_session(sqlite3 *db, int64_t requested_id, int new_session) {
    if (new_session)
        return session_create(db, "cli", NULL, -1, 0);

    if (requested_id > 0)
        return requested_id;

    /* Query sessions with first/last user prompt snippets */
    const char *sql =
        "SELECT s.id, s.created_at,"
        " (SELECT substr(e.content,1,50) FROM entries e WHERE e.session_id=s.id AND e.role=1 ORDER BY e.id ASC LIMIT 1),"
        " (SELECT substr(e.content,1,50) FROM entries e WHERE e.session_id=s.id AND e.role=1 ORDER BY e.id DESC LIMIT 1)"
        " FROM sessions s ORDER BY s.updated_at DESC;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return session_create(db, "cli", NULL, -1, 0);

    /* Collect results */
    int cap = 8, count = 0;
    int64_t *ids = malloc((size_t)cap * sizeof(int64_t));
    if (!ids) { sqlite3_finalize(stmt); return -1; }

    /* Display header (buffer rows for non-interactive mode) */
    typedef struct { int64_t id; time_t created; char first[52]; char last[52]; } Row;
    Row *rows = malloc((size_t)cap * sizeof(Row));
    if (!rows) { free(ids); sqlite3_finalize(stmt); return -1; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            ids = realloc(ids, (size_t)cap * sizeof(int64_t));
            rows = realloc(rows, (size_t)cap * sizeof(Row));
            if (!ids || !rows) break;
        }
        rows[count].id = sqlite3_column_int64(stmt, 0);
        rows[count].created = (time_t)sqlite3_column_int64(stmt, 1);
        const char *fp = (const char *)sqlite3_column_text(stmt, 2);
        const char *lp = (const char *)sqlite3_column_text(stmt, 3);
        snprintf(rows[count].first, sizeof(rows[count].first), "%s", fp ? fp : "");
        snprintf(rows[count].last, sizeof(rows[count].last), "%s", lp ? lp : "");
        ids[count] = rows[count].id;
        count++;
    }
    sqlite3_finalize(stmt);

    if (count == 0) {
        free(ids); free(rows);
        if (!isatty(STDIN_FILENO)) {
            fprintf(stderr, "no sessions. use --new to create one.\n");
            return -1;
        }
        return session_create(db, "cli", NULL, -1, 0);
    }

    if (!isatty(STDIN_FILENO)) {
        int show = count < 10 ? count : 10;
        for (int i = 0; i < show; i++)
            fprintf(stderr, "%lld\t%s\n", (long long)rows[i].id,
                    rows[i].first[0] ? rows[i].first : "(empty)");
        if (count > 10) fprintf(stderr, "... and %d more\n", count - 10);
        fprintf(stderr, "use --session-id=N or --new\n");
        free(ids); free(rows);
        return -1;
    }

    printf("sessions:\n");
    for (int i = 0; i < count; i++) {
        char timebuf[20];
        struct tm tm;
        localtime_r(&rows[i].created, &tm);
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M", &tm);
        /* Replace newlines in snippets with spaces for display */
        for (char *p = rows[i].first; *p; p++) if (*p == '\n') *p = ' ';
        for (char *p = rows[i].last; *p; p++) if (*p == '\n') *p = ' ';
        printf("  %d) [%lld] %s | %s | %s\n", i + 1, (long long)rows[i].id,
               timebuf, rows[i].first[0] ? rows[i].first : "(empty)",
               rows[i].last[0] ? rows[i].last : "");
    }
    printf("  n) new session\n");
    printf("select: ");
    fflush(stdout);

    char buf[32];
    if (!fgets(buf, sizeof(buf), stdin)) {
        free(ids); free(rows);
        return -1;
    }

    int64_t result;
    if (buf[0] == 'n' || buf[0] == 'N')
        result = session_create(db, "cli", NULL, -1, 0);
    else {
        int choice = atoi(buf);
        if (choice >= 1 && choice <= count)
            result = ids[choice - 1];
        else { fprintf(stderr, "invalid choice\n"); result = -1; }
    }

    free(ids); free(rows);
    return result;
}

/* Ensure directory for path exists */
static void ensure_parent_dir(const char *path) {
    char *dup = strdup(path);
    if (!dup) return;
    char *slash = strrchr(dup, '/');
    if (slash) {
        *slash = '\0';
        char cmd[4096];
        snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dup);
        (void)system(cmd);
    }
    free(dup);
}

/* Resolve cclaw.db path */
static char *resolve_db_path(void) {
    const char *env = getenv("CCLAW_DB_PATH");
    if (env) return strdup(env);
    const char *home = getenv("HOME");
    if (home) {
        size_t len = strlen(home);
        char *p = malloc(len + sizeof("/.cclaw/cclaw.db"));
        if (p) { sprintf(p, "%s/.cclaw/cclaw.db", home); return p; }
    }
    return strdup("cclaw.db");
}

static void print_usage(void) {
    printf("usage: cclaw [options]\n"
           "\n"
           "modes (default: interactive CLI):\n"
           "  --daemon           run as daemon (telegram, web, cron)\n"
           "  llm                run one LLM call (internal, used by parent turn loop)\n"
           "\n"
           "options:\n"
           "  -p <prompt>        single-turn: send prompt, print response, exit\n"
           "  -s <id>            session id (short for --session-id=N)\n"
           "  -y                 yolo mode: no sandbox, all hosts allowed\n"
           "  --new              create a new session\n"
           "  --log-level=LEVEL  set log level (error|info|debug|trace)\n"
           "  --help             show this help\n");
}

/* V96/T233: journal handle removed — single DB */
static sqlite3 *g_db;  /* T297: single DB handle for journal writes in CLI */
static const Config *g_cli_cfg;

int main(int argc, char *argv[]) {
    int daemon_mode = 0;
    int llm_mode = 0;
    LogLevel log_level_override = LOG_LEVEL_INFO;
    int log_level_set = 0;
    int new_session = 0;
    int yolo_mode = 0;
    int64_t session_id = -1;
    const char *prompt = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
        } else if (strcmp(argv[i], "--daemon") == 0) {
            daemon_mode = 1;
        } else if (strcmp(argv[i], "llm") == 0) {
            llm_mode = 1;
        } else if (strncmp(argv[i], "--log-level=", 12) == 0) {
            log_level_override = log_level_parse(argv[i] + 12);
            log_level_set = 1;
        } else if (strcmp(argv[i], "--new") == 0) {
            new_session = 1;
        } else if (strcmp(argv[i], "-y") == 0) {
            yolo_mode = 1;
        } else if (strcmp(argv[i], "-p") == 0) {
            if (++i >= argc) { fprintf(stderr, "error: -p requires an argument\n"); return 1; }
            prompt = argv[i];
        } else if (strcmp(argv[i], "-s") == 0) {
            if (++i >= argc) { fprintf(stderr, "error: -s requires an argument\n"); return 1; }
            session_id = atoll(argv[i]);
        } else if (strncmp(argv[i], "--session-id=", 13) == 0) {
            session_id = atoll(argv[i] + 13);
        } else {
            fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
            print_usage();
            return 1;
        }
    }

    /* T299: llm subcommand — run single LLM call */
    if (llm_mode) {
        if (session_id < 0) {
            fprintf(stderr, "error: llm requires -s <id>\n");
            return 1;
        }
        return llm_child_main(session_id);
    }

    shutdown_init();

    /* Daemon mode */
    if (daemon_mode) {
        char *db_path = resolve_db_path();
        sqlite3 *db = db_open(db_path);
        if (!db) {
            fprintf(stderr, "error: cannot open database '%s'\n", db_path);
            free(db_path);
            return 1;
        }

        uint8_t secret_key[32];
        if (secret_key_load_or_create(db_path, secret_key) == 0)
            db_set_secret_key(secret_key);
        free(db_path);

        Config *cfg = config_load(db);
        if (!cfg) {
            fprintf(stderr, "error: failed to load config from database\n");
            db_close(db);
            return 1;
        }
        if (log_level_set) cfg->log_level = log_level_override;

        workspace_init(cfg);
        printf("cclaw %s — daemon mode\n", CCLAW_VERSION);

        /* T247/V103: Register telegram as channel process if token configured */
        if (cfg->telegram_token && cfg->telegram_token[0] != '\0') {
            daemon_register_telegram_channel(db, cfg->telegram_token);
            printf("telegram channel registered\n");
        }

        if (web_start(cfg, db) != 0)
            fprintf(stderr, "warning: failed to start web server on port %d\n", cfg->web_port);
        else
            printf("web server started on port %d\n", cfg->web_port);

        if (heartbeat_start(cfg, db) != 0)
            fprintf(stderr, "warning: failed to start heartbeat timer\n");
        else if (cfg->heartbeat_interval > 0)
            printf("heartbeat started (%ds interval)\n", cfg->heartbeat_interval);

        if (cron_start(cfg, db) != 0)
            fprintf(stderr, "warning: failed to start cron scheduler\n");
        else
            printf("cron scheduler started\n");

        int64_t bootstrap_sid = daemon_bootstrap(db);
        if (bootstrap_sid > 0)
            printf("bootstrap agent created (session %lld)\n", (long long)bootstrap_sid);

        daemon_run(cfg, db);

        printf("\nshutting down...\n");
        cron_stop();
        heartbeat_stop();
        web_stop();
        db_close(db);
        config_free(cfg);
        return 0;
    }

    /* --- CLI mode (default) — mini-daemon with terminal as channel --- */

    char *db_path = resolve_db_path();
    ensure_parent_dir(db_path);
    sqlite3 *cclaw_db = db_open(db_path);
    if (!cclaw_db) {
        fprintf(stderr, "error: cannot open database '%s'\n", db_path);
        free(db_path);
        return 1;
    }

    {
        uint8_t sk[32];
        if (secret_key_load_or_create(db_path, sk) == 0)
            db_set_secret_key(sk);
    }

    /* Derive base_dir from db_path (directory containing cclaw.db) */
    char *base_dir = strdup(db_path);
    {
        char *slash = strrchr(base_dir, '/');
        if (slash) *slash = '\0';
        else { free(base_dir); base_dir = strdup("."); }
    }
    char *db_path_resolved = db_path; /* keep for CCLAW_AGENT_DB env */

    Config *cfg = config_load(cclaw_db);
    if (!cfg) {
        fprintf(stderr, "error: failed to load config\n");
        free(base_dir);
        db_close(cclaw_db);
        return 1;
    }
    if (log_level_set) cfg->log_level = log_level_override;

    if (!cfg->provider.api_key || !cfg->provider.api_key[0]) {
        fprintf(stderr, "error: no API key configured (set OPENROUTER_API_KEY or add to cclaw.db)\n");
        free(base_dir);
        config_free(cfg);
        db_close(cclaw_db);
        return 1;
    }

    /* T271/V117: Ensure "default" agent exists on first run */
    {
        int agent_count = 0;
        char **agents = db_agent_list(cclaw_db, &agent_count);
        if (!agents || agent_count == 0) {
            /* First run: create "default" agent + workspace dir */
            char ws_path[PATH_MAX];
            snprintf(ws_path, sizeof(ws_path), "%s/agents/default/workspace/.keep", base_dir);
            ensure_parent_dir(ws_path);
            db_agent_upsert(cclaw_db, "default", NULL, NULL, NULL);
            db_kv_set(cclaw_db, "default_agent", "default");
        }
        if (agents) {
            for (int i = 0; i < agent_count; i++) free(agents[i]);
            free(agents);
        }
    }

    /* T271/V118: Agent selection — -p uses default_agent; interactive shows picker */
    char *agent_name_sel = NULL;

    if (prompt || !isatty(STDIN_FILENO)) {
        /* Non-interactive / -p: use kv default_agent */
        char *def = db_kv_get(cclaw_db, "default_agent");
        agent_name_sel = def ? def : strdup("default");
    } else {
        /* Interactive: show agent picker */
        int agent_count = 0;
        char **agents = db_agent_list(cclaw_db, &agent_count);
        char *default_agent = db_kv_get(cclaw_db, "default_agent");

        if (agent_count == 1) {
            /* Single agent — auto-select */
            agent_name_sel = strdup(agents[0]);
        } else {
            printf("agents:\n");
            for (int i = 0; i < agent_count; i++) {
                int is_default = default_agent && strcmp(agents[i], default_agent) == 0;
                printf("  %d) %s%s\n", i + 1, agents[i], is_default ? " *" : "");
            }
            printf("  n) new agent...\n");
            printf("select: ");
            fflush(stdout);

            char buf[128];
            if (fgets(buf, sizeof(buf), stdin)) {
                if (buf[0] == 'n' || buf[0] == 'N') {
                    printf("agent name: ");
                    fflush(stdout);
                    if (fgets(buf, sizeof(buf), stdin)) {
                        size_t len = strlen(buf);
                        if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
                        if (buf[0]) {
                            char ws_path[PATH_MAX];
                            snprintf(ws_path, sizeof(ws_path),
                                     "%s/agents/%s/workspace/.keep", base_dir, buf);
                            ensure_parent_dir(ws_path);
                            db_agent_upsert(cclaw_db, buf, NULL, NULL, NULL);
                            agent_name_sel = strdup(buf);
                        }
                    }
                } else {
                    int choice = atoi(buf);
                    if (choice >= 1 && choice <= agent_count)
                        agent_name_sel = strdup(agents[choice - 1]);
                }
            }
        }
        free(default_agent);
        if (agents) {
            for (int i = 0; i < agent_count; i++) free(agents[i]);
            free(agents);
        }
    }

    if (!agent_name_sel) {
        fprintf(stderr, "error: no agent selected\n");
        free(base_dir);
        config_free(cfg);
        db_close(cclaw_db);
        return 1;
    }

    /* T297: single DB — sessions/entries/inbox all in cclaw_db */
    sqlite3 *adb = cclaw_db;

    /* Set env vars for child agent processes */
    setenv("CCLAW_AGENT_DB", db_path_resolved, 1);

    const char *agent_name = agent_name_sel;
    setenv("CCLAW_AGENT_NAME", agent_name, 1);

    /* T275/V119: inject CCLAW_TOOLS from agent_config (or leave unset for default) */
    if (!yolo_mode) {
        AgentConfig *ac = agent_config_load_db(cclaw_db, agent_name);
        if (ac) {
            if (ac->tool_count > 0) {
                size_t len = 0;
                for (size_t i = 0; i < ac->tool_count; i++)
                    len += strlen(ac->tools[i]) + 1;
                char *tools_csv = malloc(len);
                if (tools_csv) {
                    tools_csv[0] = '\0';
                    for (size_t i = 0; i < ac->tool_count; i++) {
                        if (i > 0) strcat(tools_csv, ",");
                        strcat(tools_csv, ac->tools[i]);
                    }
                    setenv("CCLAW_TOOLS", tools_csv, 1);
                    free(tools_csv);
                }
            }
            if (ac->allowed_hosts_count > 0) {
                size_t len = 0;
                for (size_t i = 0; i < ac->allowed_hosts_count; i++)
                    len += strlen(ac->allowed_hosts[i]) + 1;
                char *hosts_csv = malloc(len);
                if (hosts_csv) {
                    hosts_csv[0] = '\0';
                    for (size_t i = 0; i < ac->allowed_hosts_count; i++) {
                        if (i > 0) strcat(hosts_csv, ",");
                        strcat(hosts_csv, ac->allowed_hosts[i]);
                    }
                    setenv("CCLAW_ALLOWED_HOSTS", hosts_csv, 1);
                    free(hosts_csv);
                }
            }
            agent_config_free(ac);
        }
        /* T301/V22a: inject CCLAW_SANDBOX from agent_config */
        {
            char *val = NULL;
            sqlite3_stmt *s;
            const char *sql = "SELECT value FROM agent_config WHERE agent_name=? AND key='sandbox';";
            if (sqlite3_prepare_v2(cclaw_db, sql, -1, &s, NULL) == SQLITE_OK) {
                sqlite3_bind_text(s, 1, agent_name, -1, SQLITE_STATIC);
                if (sqlite3_step(s) == SQLITE_ROW)
                    val = strdup((const char *)sqlite3_column_text(s, 0));
                sqlite3_finalize(s);
            }
            setenv("CCLAW_SANDBOX", val ? val : "sandbox", 1);
            free(val);
        }
    }

    if (yolo_mode) setenv("CCLAW_YOLO", "1", 1);

    /* Inject log level for child agent processes */
    const char *level_str = cfg->log_level == LOG_LEVEL_TRACE ? "trace" :
                            cfg->log_level == LOG_LEVEL_DEBUG ? "debug" :
                            cfg->log_level == LOG_LEVEL_ERROR ? "error" : "info";
    setenv("CCLAW_LOG_LEVEL", level_str, 1);

    /* CLI mode: enable SSE streaming by default (user can override with CCLAW_STREAM=0) */
    if (!getenv("CCLAW_STREAM")) {
        setenv("CCLAW_STREAM", "1", 1);
        cfg->stream = 1;
    }
    setenv("CCLAW_MODE", "cli", 1);

    /* T297: single DB handles journal writes too */
    g_db = cclaw_db;
    g_cli_cfg = cfg;

    /* T228: CWD as read-only path for agent file_read */
    {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)))
            setenv("CCLAW_PATH", cwd, 1);
    }

    workspace_init(cfg);

    session_id = cli_select_session(adb, session_id, new_session);
    if (session_id < 0) {
        free(agent_name_sel);
        free(base_dir);
        free(db_path_resolved);
        config_free(cfg);
        db_close(cclaw_db);
        return 1;
    }

    int unread = inbox_count(adb, session_id);
    if (unread > 0)
        printf("[%d unread inbox message%s]\n", unread, unread == 1 ? "" : "s");

    int rc = 0;

    /* Init tool setup for this session */
    AgentSetup setup;
    agent_setup_init(&setup, adb, session_id, cfg, agent_name, NULL, 0, AGENT_SETUP_CLI);

    /* Ensure system prompt exists */
    {
        int branch_count = 0;
        Entry *branch = session_get_branch(adb, session_id, &branch_count);
        if (branch_count == 0) {
            char *prompt_txt = agent_build_system_prompt(adb, agent_name, session_id,
                                                        "agents", cfg);
            Message sys_msg = {.role = ROLE_SYSTEM, .content = prompt_txt};
            entry_append(adb, session_id, &sys_msg);
            free(prompt_txt);
        }
        entry_branch_free(branch, branch_count);
    }

    /* Single-turn mode: -p <prompt> */
    if (prompt) {
        inbox_insert(adb, session_id, "cli", prompt);
        rc = turn_loop_run(adb, session_id, cfg, &setup, TURN_MODE_CLI);
        if (rc == 0) {
            /* Streaming: child wrote tokens to stdout; just add trailing newline.
             * Non-streaming: print full response from DB. */
            if (cfg->stream)
                printf("\n");
            else
                print_response(adb, session_id);
        }
        goto done;
    }

    /* Interactive REPL */
    printf("cclaw cli (type 'exit' or Ctrl-D to quit)\n");

    /* Clear stale CLI inbox messages (from crashed turns). Ignore non-CLI
     * inbox messages — those belong to another channel (daemon handles). */
    if (unread > 0) {
        int cleared = inbox_clear_source(adb, session_id, "cli");
        if (cleared > 0)
            printf("[cleared %d stale prompt%s]\n", cleared, cleared == 1 ? "" : "s");
    }

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;

    while (!shutdown_requested()) {
        printf("> ");
        fflush(stdout);

        line_len = getline(&line, &line_cap, stdin);
        if (line_len < 0) break;
        if (shutdown_requested()) break;

        if (line_len > 0 && line[line_len - 1] == '\n')
            line[line_len - 1] = '\0';
        if (line[0] == '\0') continue;
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) break;

        inbox_insert(adb, session_id, "cli", line);
        rc = turn_loop_run(adb, session_id, cfg, &setup, TURN_MODE_CLI);
        if (rc == 0) {
            if (cfg->stream)
                printf("\n");
            else
                print_response(adb, session_id);
        }
    }

    free(line);

done:
    /* T268: print session cost on exit */
    {
        int64_t cost = session_cost(adb, session_id);
        if (cost > 0) {
            double dollars = (double)cost / 1e9;
            fprintf(stderr, "\n[session cost: $%.6f]\n", dollars);
        }
    }

    session_set_state(adb, session_id, "idle");
    agent_setup_destroy(&setup);
    free(agent_name_sel);
    free(base_dir);
    free(db_path_resolved);
    config_free(cfg);
    db_close(cclaw_db);
    return rc == 0 ? 0 : 1;
}
