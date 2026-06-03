#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <limits.h>
#include <libgen.h>
#include "cclaw.h"
#include "config.h"
#include "log.h"
#include "agent_config.h"
#include "agent_exit.h"
#include "agent_turn.h"
#include "web.h"
#include "heartbeat.h"
#include "cron.h"
#include "db.h"
#include "shutdown.h"
#include "context.h"
#include "daemon.h"
#include "secret.h"

/* Print last assistant response from session branch */
static void print_response(sqlite3 *db, int64_t session_id) {
    char *text = get_response_text(db, session_id);
    if (text) {
        printf("%s\n", text);
        free(text);
    }
}

/* Prompt user to select or create a session. Returns session id or -1. */
static int64_t cli_select_session(sqlite3 *db, int64_t requested_id, int new_session) {
    if (new_session)
        return session_create(db, "cli", NULL, -1, 0);

    if (requested_id > 0)
        return requested_id;

    if (!isatty(STDIN_FILENO)) {
        int count = 0;
        Session *sessions = session_list(db, &count);
        if (!sessions || count == 0) {
            fprintf(stderr, "no sessions. use --new to create one.\n");
        } else {
            int show = count < 10 ? count : 10;
            for (int i = 0; i < show; i++)
                fprintf(stderr, "%lld\t%s\n", (long long)sessions[i].id,
                        sessions[i].name ? sessions[i].name : "(unnamed)");
            if (count > 10) fprintf(stderr, "... and %d more\n", count - 10);
            fprintf(stderr, "use --session-id=N or --new\n");
        }
        session_list_free(sessions, count);
        return -1;
    }

    int count = 0;
    Session *sessions = session_list(db, &count);

    if (!sessions || count == 0) {
        session_list_free(sessions, count);
        return session_create(db, "cli", NULL, -1, 0);
    }

    printf("sessions:\n");
    for (int i = 0; i < count; i++)
        printf("  %d) [%lld] %s\n", i + 1, (long long)sessions[i].id,
               sessions[i].name ? sessions[i].name : "(unnamed)");
    printf("  n) new session\n");
    printf("select: ");
    fflush(stdout);

    char buf[32];
    if (!fgets(buf, sizeof(buf), stdin)) {
        session_list_free(sessions, count);
        return -1;
    }

    int64_t result;
    if (buf[0] == 'n' || buf[0] == 'N')
        result = session_create(db, "cli", NULL, -1, 0);
    else {
        int choice = atoi(buf);
        if (choice >= 1 && choice <= count)
            result = sessions[choice - 1].id;
        else { fprintf(stderr, "invalid choice\n"); result = -1; }
    }

    session_list_free(sessions, count);
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
           "  --agent            run one agent turn (internal, used by daemon)\n"
           "\n"
           "options:\n"
           "  -p <prompt>        single-turn: send prompt, print response, exit\n"
           "  -s <id>            session id (short for --session-id=N)\n"
           "  -y                 yolo mode: no sandbox, all hosts allowed\n"
           "  --new              create a new session\n"
           "  --log-level=LEVEL  set log level (error|info|debug|trace)\n"
           "  --help             show this help\n");
}

/* V96/T233: journal.db handle for CLI log persistence */
static sqlite3 *g_journal_db;
static const Config *g_cli_cfg;

/* Fork agent_turn_run as child, pipe stderr → journal.db + tee to terminal.
 * stdout inherited (future: streaming tokens). */
static int cli_fork_turn(int64_t session_id) {
    int err_pipe[2];
    if (pipe(err_pipe) != 0) {
        perror("pipe");
        return AGENT_EXIT_ERROR;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        close(err_pipe[0]); close(err_pipe[1]);
        return AGENT_EXIT_ERROR;
    }
    if (pid == 0) {
        /* Child: stderr → pipe, stdout inherited */
        close(err_pipe[0]);
        dup2(err_pipe[1], STDERR_FILENO);
        close(err_pipe[1]);
        _exit(agent_turn_run(session_id));
    }

    /* Parent: drain stderr pipe → journal.db + optional tee */
    close(err_pipe[1]);

    int tee = g_cli_cfg && g_cli_cfg->log_level >= LOG_LEVEL_DEBUG;
    const char *agent_name = getenv("CCLAW_AGENT_NAME");
    if (!agent_name) agent_name = "default";

    /* Prepare journal insert if DB available */
    sqlite3_stmt *stmt = NULL;
    if (g_journal_db) {
        sqlite3_prepare_v2(g_journal_db,
            "INSERT INTO log(source, pid, session_id, stream, line) VALUES(?,?,?,2,?)",
            -1, &stmt, NULL);
    }

    FILE *fp = fdopen(err_pipe[0], "r");
    if (fp) {
        char buf[4096];
        while (fgets(buf, sizeof(buf), fp)) {
            /* Strip trailing newline for DB storage */
            size_t len = strlen(buf);
            if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';

            if (stmt) {
                sqlite3_bind_text(stmt, 1, agent_name, -1, SQLITE_STATIC);
                sqlite3_bind_int(stmt, 2, pid);
                sqlite3_bind_int64(stmt, 3, session_id);
                sqlite3_bind_text(stmt, 4, buf, -1, SQLITE_STATIC);
                sqlite3_step(stmt);
                sqlite3_reset(stmt);
            }
            if (tee) fprintf(stderr, "%s\n", buf);
        }
        fclose(fp);
    } else {
        close(err_pipe[0]);
    }

    if (stmt) sqlite3_finalize(stmt);

    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return AGENT_EXIT_ERROR;
}

/* Handle exit code 3: approval request. Prompt user, write result, re-fork. */
static int cli_handle_approval(sqlite3 *adb, int64_t session_id) {
    int64_t pending_eid = 0;
    char *tc_id = find_pending_entry(adb, session_id, &pending_eid);
    if (!tc_id) return -1;

    char *tname = NULL;
    char *args = read_tool_call_args(adb, session_id, tc_id, &tname);
    if (!args) { free(tc_id); return -1; }

    /* Display approval request */
    printf("\033[33m⚠ approval requested");
    if (tname) printf(" [%s]", tname);
    printf(":\033[0m %s\n", args);
    printf("allow? [y/n]: ");
    fflush(stdout);

    char buf[16];
    const char *result;
    if (fgets(buf, sizeof(buf), stdin) && (buf[0] == 'y' || buf[0] == 'Y'))
        result = "approved";
    else
        result = "denied";

    update_pending_entry(adb, pending_eid, result);
    free(args);
    free(tname);
    free(tc_id);
    return 0;
}

/* Handle exit code 4: config change. Apply and resolve PENDING. */
static int cli_handle_config(const Config *cfg, sqlite3 *cclaw_db,
                             sqlite3 *adb, int64_t session_id,
                             const char *agent_name) {
    int64_t pending_eid = 0;
    char *tc_id = find_pending_entry(adb, session_id, &pending_eid);
    if (!tc_id) return -1;

    char *tname = NULL;
    char *args = read_tool_call_args(adb, session_id, tc_id, &tname);
    if (!args) { free(tc_id); return -1; }

    char *result = daemon_apply_config(cfg, cclaw_db, agent_name, tname, args);
    update_pending_entry(adb, pending_eid, result ? result : "error: config apply failed");

    printf("\033[36mconfig: %s\033[0m\n", result ? result : "failed");
    free(result);
    free(args);
    free(tname);
    free(tc_id);
    return 0;
}

/* Run a turn: fork agent, dispatch on exit code. Returns 0 when turn is done. */
static int cli_run_turn(sqlite3 *adb, sqlite3 *cclaw_db, const Config *cfg,
                        int64_t session_id, const char *agent_name) {
    for (;;) {
        int rc = cli_fork_turn(session_id);

        switch (rc) {
        case AGENT_EXIT_DONE:
            return 0;
        case AGENT_EXIT_ERROR:
            fprintf(stderr, "error: agent failed\n");
            return 1;
        case AGENT_EXIT_APPROVAL:
            if (cli_handle_approval(adb, session_id) != 0) return 1;
            /* Re-fork to continue after approval */
            session_set_state(adb, session_id, "running");
            continue;
        case AGENT_EXIT_CONFIG:
            if (cli_handle_config(cfg, cclaw_db, adb, session_id, agent_name) != 0)
                return 1;
            /* Re-fork to continue after config applied */
            session_set_state(adb, session_id, "running");
            continue;
        case AGENT_EXIT_SPAWN:
            /* For now: print and continue (sub-agent spawning is daemon territory) */
            printf("\033[33m⚠ agent requested spawn (not supported in CLI mode)\033[0m\n");
            {
                int64_t pending_eid = 0;
                char *tc_id = find_pending_entry(adb, session_id, &pending_eid);
                if (tc_id) {
                    update_pending_entry(adb, pending_eid,
                        "error: spawn not available in CLI mode");
                    free(tc_id);
                }
            }
            session_set_state(adb, session_id, "running");
            continue;
        default:
            fprintf(stderr, "error: unexpected exit code %d\n", rc);
            return 1;
        }
    }
}

int main(int argc, char *argv[]) {
    int daemon_mode = 0;
    int agent_mode = 0;
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
        } else if (strcmp(argv[i], "--agent") == 0) {
            agent_mode = 1;
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

    /* --agent: separate program, self-contained */
    if (agent_mode) {
        if (session_id < 0) {
            fprintf(stderr, "error: --agent requires -s <id>\n");
            return 1;
        }
        return agent_turn_run(session_id);
    }

    shutdown_init();

    /* Daemon mode */
    if (daemon_mode) {
        char *db_path = resolve_db_path();
        sqlite3 *db = db_open_cclaw(db_path);
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
    sqlite3 *cclaw_db = db_open_cclaw(db_path);
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
    free(db_path);

    Config *cfg = config_load(cclaw_db);
    if (!cfg) {
        fprintf(stderr, "error: failed to load config\n");
        db_close(cclaw_db);
        return 1;
    }
    if (log_level_set) cfg->log_level = log_level_override;

    if (!cfg->provider.api_key || !cfg->provider.api_key[0]) {
        fprintf(stderr, "error: no API key configured (set OPENROUTER_API_KEY or add to cclaw.db)\n");
        config_free(cfg);
        db_close(cclaw_db);
        return 1;
    }

    /* Open agent DB */
    const char *agent_db_env = getenv("CCLAW_AGENT_DB");
    char *agent_db_path = agent_db_env && agent_db_env[0]
        ? strdup(agent_db_env) : strdup(".cclaw/agents/default/agent.db");
    ensure_parent_dir(agent_db_path);
    sqlite3 *adb = db_open_agent(agent_db_path);
    if (!adb) {
        fprintf(stderr, "error: cannot open agent database '%s'\n", agent_db_path);
        free(agent_db_path);
        config_free(cfg);
        db_close(cclaw_db);
        return 1;
    }

    {
        uint8_t sk[32];
        if (secret_key_load_or_create(agent_db_path, sk) == 0)
            db_set_secret_key(sk);
    }

    /* Set env vars for child agent processes */
    setenv("CCLAW_AGENT_DB", agent_db_path, 1);
    free(agent_db_path);

    const char *agent_name_env = getenv("CCLAW_AGENT_NAME");
    const char *agent_name = agent_name_env ? agent_name_env : "default";
    setenv("CCLAW_AGENT_NAME", agent_name, 0);

    if (yolo_mode) setenv("CCLAW_YOLO", "1", 1);

    /* Inject log level for child agent processes */
    const char *level_str = cfg->log_level == LOG_LEVEL_TRACE ? "trace" :
                            cfg->log_level == LOG_LEVEL_DEBUG ? "debug" :
                            cfg->log_level == LOG_LEVEL_ERROR ? "error" : "info";
    setenv("CCLAW_LOG_LEVEL", level_str, 1);

    /* CLI mode: enable SSE streaming for real-time token output */
    setenv("CCLAW_STREAM", "1", 1);
    setenv("CCLAW_MODE", "cli", 1);
    cfg->stream = 1;

    /* V96/T233: open journal.db for CLI log persistence */
    g_journal_db = db_open_journal(".cclaw/journal.db");
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
        db_close(adb);
        config_free(cfg);
        db_close(cclaw_db);
        return 1;
    }

    int unread = inbox_count(adb, session_id);
    if (unread > 0)
        printf("[%d unread inbox message%s]\n", unread, unread == 1 ? "" : "s");

    int rc = 0;

    /* Single-turn mode: -p <prompt> */
    if (prompt) {
        inbox_insert(adb, session_id, "cli", prompt);
        rc = cli_run_turn(adb, cclaw_db, cfg, session_id, agent_name);
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
        rc = cli_run_turn(adb, cclaw_db, cfg, session_id, agent_name);
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
    db_close(adb);
    if (g_journal_db) db_close(g_journal_db);
    config_free(cfg);
    db_close(cclaw_db);
    return rc == 0 ? 0 : 1;
}
