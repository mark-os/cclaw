#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cclaw.h"
#include "config.h"
#include "log.h"
#include "agent_config.h"
#include "agent_setup.h"
#include "llm_proc.h"
#include "llm_worker.h"
#include "tools.h"
#include "context.h"
#include "db.h"
#include "templates.h"
#include "db_response.h"
#include "shutdown.h"
#include "wake.h"
#include "channel.h"
#include "channel_api.h"
#include "secret.h"
#include "web.h"
#include "heartbeat.h"
#include "cron.h"

/* ── Constants ──────────────────────────────────────────────────── */

#define CHILD_MAX 48
#define TOOL_MAX_OUTPUT (60 * 1024)  /* 60KB — fits in pipe buffer */
#define DEFAULT_MAX_ITERATIONS 25

/* ── Child types and tracking ───────────────────────────────────── */

typedef enum { CHILD_CHANNEL, CHILD_LLM_REQ, CHILD_TOOL_EXEC } ChildType;

typedef struct {
    pid_t pid;
    ChildType type;
    int64_t session_id;
    char agent_name[64];
    /* LLM_REQ fields */
    int iteration;
    /* TOOL_EXEC fields */
    char tool_call_id[64];
    int64_t turn_id;
    int64_t entry_id;       /* tool_call entry id */
    int result_pipe;        /* read end of pipe for tool output */
    /* Channel fields */
    char channel_name[64];
    char binary_path[512];
    int restart_count;
} ChildProc;

static ChildProc g_children[CHILD_MAX];
static int g_child_count;

static ChildProc *child_find(pid_t pid) {
    for (int i = 0; i < g_child_count; i++)
        if (g_children[i].pid == pid) return &g_children[i];
    return NULL;
}

static void child_remove(ChildProc *c) {
    int idx = (int)(c - g_children);
    if (idx < 0 || idx >= g_child_count) return;
    g_children[idx] = g_children[g_child_count - 1];
    g_child_count--;
}

static int child_has_session(int64_t session_id) {
    for (int i = 0; i < g_child_count; i++)
        if ((g_children[i].type == CHILD_LLM_REQ || g_children[i].type == CHILD_TOOL_EXEC)
            && g_children[i].session_id == session_id)
            return 1;
    return 0;
}

/* ── Globals ────────────────────────────────────────────────────── */

/* Forward declarations */
static int fork_llm_req(int64_t session_id, const char *agent_name, int iteration);
static void deliver_response(int64_t session_id);
static int fork_tool_exec(int64_t session_id, const char *agent_name, PendingToolCall *tc);

static sqlite3 *g_db;
static Config *g_cfg;
static int g_mode;  /* 0=cli, 1=daemon */
static int g_llm_fork;             /* 1 = old fork-per-turn, 0 = worker mode */
static int g_llm_threads = 2;     /* worker thread pool size */
static int64_t g_cli_session;
static char g_agent_name[64];
static int g_cli_turn_active;   /* 1 while CLI is waiting for a turn to finish */
static int g_cli_done;          /* 1 = exit after turn completes (for -p mode) */

/* SIGCHLD self-pipe */
static int g_chld_pipe[2] = {-1, -1};

static void sigchld_handler(int sig) {
    (void)sig;
    char c = 1;
    (void)write(g_chld_pipe[1], &c, 1);
}

static void set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ── fork_llm_req ───────────────────────────────────────────────── */

/* Handle LLM completion: query DB, dispatch tools or deliver response */
static void handle_llm_complete(int64_t session_id, const char *agent_name, int iteration);

/* Dispatch LLM request via worker or fork depending on mode */
static int dispatch_llm_req(int64_t session_id, const char *agent_name, int iteration) {
    if (!g_llm_fork && llm_worker_alive()) {
        /* Worker mode: submit to worker, track session state */
        if (child_has_session(session_id)) return -1;

        int max_iter = g_cfg->max_iterations > 0 ? g_cfg->max_iterations : DEFAULT_MAX_ITERATIONS;
        if (iteration >= max_iter) {
            int64_t turn_id = db_next_turn_id(g_db, session_id);
            Message msg = {.role = ROLE_ASSISTANT,
                           .content = "error: max iterations reached",
                           .stop_reason = STOP_REASON_ERROR};
            entry_append_with_turn(g_db, session_id, &msg, turn_id);
            session_set_state(g_db, session_id, "idle");
            return -1;
        }

        session_set_state(g_db, session_id, "llm_running");

        /* Track iteration in a lightweight way for worker callbacks */
        for (int i = 0; i < g_child_count; i++) {
            if (g_children[i].session_id == session_id) {
                g_children[i].iteration = iteration;
                break;
            }
        }
        /* Add tracking entry if not present */
        if (!child_has_session(session_id) && g_child_count < CHILD_MAX) {
            ChildProc *c = &g_children[g_child_count++];
            memset(c, 0, sizeof(*c));
            c->pid = -1; /* sentinel: worker-managed */
            c->type = CHILD_LLM_REQ;
            c->session_id = session_id;
            c->iteration = iteration;
            c->result_pipe = -1;
            snprintf(c->agent_name, sizeof(c->agent_name), "%s", agent_name);
        }

        return llm_worker_submit(g_db, session_id, agent_name, iteration == 0 ? 1 : 0);
    }
    /* Fork mode */
    return fork_llm_req(session_id, agent_name, iteration);
}

static int fork_llm_req(int64_t session_id, const char *agent_name, int iteration) {
    if (child_has_session(session_id)) return -1;
    if (g_child_count >= CHILD_MAX) return -1;

    /* Rate limit check */
    if (g_cfg->token_rate_limit > 0 && !rate_limit_check(g_db, NULL)) {
        session_set_state(g_db, session_id, "rate_limited");
        return -1;
    }

    int max_iter = g_cfg->max_iterations > 0 ? g_cfg->max_iterations : DEFAULT_MAX_ITERATIONS;
    if (iteration >= max_iter) {
        /* Write error entry and go idle */
        int64_t turn_id = db_next_turn_id(g_db, session_id);
        Message msg = {.role = ROLE_ASSISTANT,
                       .content = "error: max iterations reached",
                       .stop_reason = STOP_REASON_ERROR};
        entry_append_with_turn(g_db, session_id, &msg, turn_id);
        session_set_state(g_db, session_id, "idle");
        return -1;
    }

    /* Set recall env for first iteration */
    setenv("CCLAW_RECALL", iteration == 0 ? "1" : "0", 1);

    session_set_state(g_db, session_id, "llm_running");

    pid_t pid = fork();
    if (pid < 0) {
        session_set_state(g_db, session_id, "idle");
        return -1;
    }
    if (pid == 0) {
        /* Child: LLM proc */
        if (g_mode == 1) {
            /* Daemon: redirect stdout to /dev/null */
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); close(devnull); }
        }
        /* CLI: inherit stdout (streaming tokens) */
        _exit(llm_proc_main(session_id));
    }

    /* Parent: track child */
    ChildProc *c = &g_children[g_child_count++];
    memset(c, 0, sizeof(*c));
    c->pid = pid;
    c->type = CHILD_LLM_REQ;
    c->session_id = session_id;
    c->iteration = iteration;
    c->result_pipe = -1;
    snprintf(c->agent_name, sizeof(c->agent_name), "%s", agent_name);
    return 0;
}

/* ── fork_tool_exec ─────────────────────────────────────────────── */

/* Tools that run in-process (need parent's DB handle or user interaction) */
static int tool_is_inline(const char *name) {
    return strcmp(name, "request_config") == 0 ||
           strcmp(name, "memory_create") == 0 ||
           strcmp(name, "memory_append") == 0 ||
           strcmp(name, "memory_replace") == 0 ||
           strcmp(name, "db_query") == 0 ||
           strcmp(name, "js_eval") == 0 ||
           strcmp(name, "js_define_tool") == 0 ||
           strcmp(name, "launch_agent") == 0 ||
           strcmp(name, "check_agent") == 0;
}

static AgentSetup *g_tool_setup;  /* Initialized once for tool dispatch */

static int fork_tool_exec(int64_t session_id, const char *agent_name,
                          PendingToolCall *tc) {
    if (g_child_count >= CHILD_MAX) return -1;

    ToolEntry *te = g_tool_setup ? tools_lookup(&g_tool_setup->reg, tc->name) : NULL;
    if (!te) {
        /* Unknown tool — write error result directly */
        char err[128];
        snprintf(err, sizeof(err), "error: unknown tool '%s'", tc->name);
        ToolResult tr = {.tool_call_id = tc->call_id, .content = err};
        Message msg = {.role = ROLE_TOOL, .tool_result = &tr,
                       .tool_name = tc->name, .is_error = 1};
        entry_append_with_turn(g_db, session_id, &msg, tc->turn_id);
        db_tool_call_set_status(g_db, session_id, tc->call_id, "done", NULL);
        return 1; /* Signal: handled inline, check for more */
    }

    /* Inline tools: execute in parent process */
    if (tool_is_inline(tc->name)) {
        char *result = te->handler(tc->arguments, te->user_data);
        if (!result) result = strdup("error: tool returned null");

        /* CLI progress */
        if (g_mode == 0) {
            fprintf(stdout, "\n\033[2m[%s]\033[0m ", tc->name);
            size_t rlen = strlen(result);
            if (rlen <= 80)
                fprintf(stdout, "\033[2m→ %s\033[0m\n", result);
            else
                fprintf(stdout, "\033[2m→ %.77s...\033[0m\n", result);
            fflush(stdout);
        }

        char *stored = truncate_and_spill(result, session_id, tc->call_id);
        ToolResult tr = {.tool_call_id = tc->call_id,
                         .content = stored ? stored : result};
        int is_err = (strncmp(result, "error:", 6) == 0);
        Message msg = {.role = ROLE_TOOL, .tool_result = &tr,
                       .tool_name = tc->name, .is_error = is_err};
        int64_t rid = entry_append_with_turn(g_db, session_id, &msg, tc->turn_id);
        db_tool_call_complete_with_result(g_db, tc->entry_id, tc->call_id, rid);
        free(stored);
        free(result);
        return 1; /* Handled inline */
    }

    /* Forkable tool: pipe for result capture */
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    /* CLI progress */
    if (g_mode == 0) {
        fprintf(stdout, "\n\033[2m[%s]\033[0m ", tc->name);
        fflush(stdout);
    }

    session_set_state(g_db, session_id, "tool_running");

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        /* Child: only the output pipe write end is inherited (no O_CLOEXEC).
         * Parent infrastructure fds (DB, sigchld pipe, notify pipe) are
         * O_CLOEXEC and will close on exec within the shell sandbox. */
        close(pipefd[0]);
        char *result = te->handler(tc->arguments, te->user_data);
        if (result) {
            size_t len = strlen(result);
            if (len > TOOL_MAX_OUTPUT) len = TOOL_MAX_OUTPUT;
            size_t written = 0;
            while (written < len) {
                ssize_t n = write(pipefd[1], result + written, len - written);
                if (n <= 0) break;
                written += (size_t)n;
            }
            free(result);
        }
        close(pipefd[1]);
        _exit(0);
    }

    /* Parent: track */
    close(pipefd[1]);
    ChildProc *c = &g_children[g_child_count++];
    memset(c, 0, sizeof(*c));
    c->pid = pid;
    c->type = CHILD_TOOL_EXEC;
    c->session_id = session_id;
    c->turn_id = tc->turn_id;
    c->entry_id = tc->entry_id;
    c->result_pipe = pipefd[0];
    snprintf(c->agent_name, sizeof(c->agent_name), "%s", agent_name);
    snprintf(c->tool_call_id, sizeof(c->tool_call_id), "%s", tc->call_id);
    return 0;
}

/* ── reap_children (state machine) ──────────────────────────────── */

/* handle_llm_complete: query DB for next action after LLM request finishes */
static void handle_llm_complete(int64_t session_id, const char *agent_name, int iteration) {
    int tc_state = turn_complete(g_db, session_id);
    if (tc_state == 1) {
        /* Has pending tool_calls — dispatch */
        int tc_count = 0;
        PendingToolCall *calls = db_tool_call_get_pending(g_db, session_id, &tc_count);
        if (tc_count > 0) {
            int i = 0;
            while (i < tc_count) {
                int rc = fork_tool_exec(session_id, agent_name, &calls[i]);
                if (rc == 1) { i++; continue; }
                break;
            }
            if (i >= tc_count)
                dispatch_llm_req(session_id, agent_name, iteration + 1);
        } else {
            session_set_state(g_db, session_id, "idle");
        }
        db_tool_call_free_pending(calls, tc_count);
    } else if (tc_state == 0) {
        /* Final stop — deliver */
        session_set_state(g_db, session_id, "idle");
        deliver_response(session_id);
    } else {
        /* Error */
        session_set_state(g_db, session_id, "idle");
        if (g_mode == 0) {
            fprintf(stderr, "error: LLM request failed\n");
            g_cli_turn_active = 0;
        }
    }
}

static void deliver_response(int64_t session_id) {
    if (g_mode == 0) {
        /* CLI: streaming wrote to stdout already, just newline */
        if (g_cfg->stream)
            printf("\n");
        else {
            char *text = get_response_text(g_db, session_id);
            if (text) { printf("%s\n", text); free(text); }
        }
        g_cli_turn_active = 0;
        return;
    }

    /* Daemon mode: read channel from session, write outbox */
    const char *src_sql = "SELECT channel_name, channel_id FROM sessions WHERE id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_db, src_sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(stmt, 1, session_id);
    char *channel = NULL, *channel_id = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *s = (const char *)sqlite3_column_text(stmt, 0);
        if (s) channel = strdup(s);
        s = (const char *)sqlite3_column_text(stmt, 1);
        if (s) channel_id = strdup(s);
    }
    sqlite3_finalize(stmt);
    if (!channel) return;

    char *text = get_response_text(g_db, session_id);
    if (!text) { free(channel); free(channel_id); return; }

    /* Build outbox payload via SQLite json_object (safe escaping) */
    const char *ins_sql =
        "INSERT INTO channel_outbox(channel_name, session_id, payload)"
        " VALUES(?1, ?2, json_object('chat_id', ?3, 'text', ?4));";
    sqlite3_stmt *ins;
    if (sqlite3_prepare_v2(g_db, ins_sql, -1, &ins, NULL) == SQLITE_OK) {
        sqlite3_bind_text(ins, 1, channel, -1, SQLITE_STATIC);
        sqlite3_bind_int64(ins, 2, session_id);
        sqlite3_bind_text(ins, 3, channel_id ? channel_id : "0", -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 4, text, -1, SQLITE_STATIC);
        sqlite3_step(ins);
        sqlite3_finalize(ins);

        const char *db_path = getenv("CCLAW_DB");
        if (db_path) channel_outbox_wake(db_path, channel);
    }

    free(text);
    free(channel);
    free(channel_id);
}

static void reap_children(void) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        /* Worker crash recovery */
        if (pid == llm_worker_pid()) {
            LOG_ERROR_(g_cfg, "LLM worker crashed, respawning");
            llm_worker_respawn();
            continue;
        }

        ChildProc *c = child_find(pid);
        if (!c) continue;

        if (c->type == CHILD_LLM_REQ) {
            (void)status; /* exit code ignored — use DB-based detection */
            int64_t session_id = c->session_id;
            char aname[64];
            snprintf(aname, sizeof(aname), "%s", c->agent_name);
            int iter = c->iteration;
            child_remove(c);

            /* DB-based completion detection (Task 2) */
            handle_llm_complete(session_id, aname, iter);
        } else if (c->type == CHILD_TOOL_EXEC) {
            int64_t session_id = c->session_id;
            char aname[64];
            snprintf(aname, sizeof(aname), "%s", c->agent_name);
            int iter = c->iteration;

            /* Read tool result from pipe */
            char *output = malloc(TOOL_MAX_OUTPUT + 1);
            size_t out_len = 0;
            if (output && c->result_pipe >= 0) {
                while (out_len < TOOL_MAX_OUTPUT) {
                    ssize_t n = read(c->result_pipe, output + out_len,
                                     TOOL_MAX_OUTPUT - out_len);
                    if (n <= 0) break;
                    out_len += (size_t)n;
                }
                output[out_len] = '\0';
            }
            if (c->result_pipe >= 0) close(c->result_pipe);
            if (!output) output = strdup("error: OOM");

            /* CLI progress */
            if (g_mode == 0) {
                if (out_len <= 80)
                    fprintf(stdout, "\033[2m→ %s\033[0m\n", output);
                else
                    fprintf(stdout, "\033[2m→ %.77s...\033[0m\n", output);
                fflush(stdout);
            }

            /* Write result to DB */
            char *stored = truncate_and_spill(output, session_id, c->tool_call_id);
            ToolResult tr = {.tool_call_id = c->tool_call_id,
                             .content = stored ? stored : output};
            int is_err = (strncmp(output, "error:", 6) == 0);
            Message msg = {.role = ROLE_TOOL, .tool_result = &tr,
                           .tool_name = "", .is_error = is_err};
            int64_t rid = entry_append_with_turn(g_db, session_id, &msg, c->turn_id);
            db_tool_call_complete_with_result(g_db, c->entry_id, c->tool_call_id, rid);
            free(stored);
            free(output);

            /* Find LLM iteration from parent child that spawned us.
             * We stored it... actually we didn't. Get from session's last LLM. */
            (void)iter;

            child_remove(c);

            /* Check for more pending tools */
            int tc_count = 0;
            PendingToolCall *calls = db_tool_call_get_pending(g_db, session_id, &tc_count);
            if (tc_count > 0) {
                /* More tools — dispatch next (inline or forked) */
                int i = 0;
                while (i < tc_count) {
                    int rc = fork_tool_exec(session_id, aname, &calls[i]);
                    if (rc == 1) { i++; continue; }
                    break;
                }
                if (i >= tc_count) {
                    /* All remaining were inline — back to LLM */
                    /* Need iteration count — derive from how many LLM turns happened */
                    int llm_iter = iter + 1;
                    dispatch_llm_req(session_id, aname, llm_iter);
                }
            } else {
                /* All tools done — back to LLM */
                int llm_iter = iter + 1;
                dispatch_llm_req(session_id, aname, llm_iter);
            }
            db_tool_call_free_pending(calls, tc_count);
        }
        /* CHILD_CHANNEL handling deferred to Task 4 */
    }
}

/* ── Helpers ────────────────────────────────────────────────────── */

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
           "  llm                run one LLM call (internal)\n"
           "\n"
           "options:\n"
           "  -p <prompt>        single-turn: send prompt, print response, exit\n"
           "  -s <id>            session id\n"
           "  -y                 yolo mode: no sandbox, all hosts allowed\n"
           "  --new              create a new session\n"
           "  --log-level=LEVEL  set log level (error|info|debug|trace)\n"
           "  -v, --debug        debug logging (timing, context stats)\n"
           "  -vv, --trace       trace logging (full req/resp JSON)\n"
           "  --help             show this help\n");
}

/* Session picker (preserved from old main.c) */
static int64_t cli_select_session(sqlite3 *db, int64_t requested_id, int new_session) {
    if (new_session) return session_create(db, "cli", NULL, -1, 0);
    if (requested_id > 0) return requested_id;

    const char *sql =
        "SELECT s.id, s.created_at,"
        " (SELECT substr(e.content,1,50) FROM entries e WHERE e.session_id=s.id AND e.role=1 ORDER BY e.id ASC LIMIT 1),"
        " (SELECT substr(e.content,1,50) FROM entries e WHERE e.session_id=s.id AND e.role=1 ORDER BY e.id DESC LIMIT 1)"
        " FROM sessions s ORDER BY s.updated_at DESC;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return session_create(db, "cli", NULL, -1, 0);

    typedef struct { int64_t id; time_t created; char first[52]; char last[52]; } Row;
    int cap = 8, count = 0;
    Row *rows = malloc((size_t)cap * sizeof(Row));
    if (!rows) { sqlite3_finalize(stmt); return -1; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) { cap *= 2; Row *tmp = realloc(rows, (size_t)cap * sizeof(Row)); if (!tmp) break; rows = tmp; }
        rows[count].id = sqlite3_column_int64(stmt, 0);
        rows[count].created = (time_t)sqlite3_column_int64(stmt, 1);
        const char *fp = (const char *)sqlite3_column_text(stmt, 2);
        const char *lp = (const char *)sqlite3_column_text(stmt, 3);
        snprintf(rows[count].first, sizeof(rows[count].first), "%s", fp ? fp : "");
        snprintf(rows[count].last, sizeof(rows[count].last), "%s", lp ? lp : "");
        count++;
    }
    sqlite3_finalize(stmt);

    if (count == 0) { free(rows); return session_create(db, "cli", NULL, -1, 0); }
    if (!isatty(STDIN_FILENO)) { int64_t r = rows[0].id; free(rows); return r; }

    printf("sessions:\n");
    for (int i = 0; i < count; i++) {
        char tb[20]; struct tm tm; localtime_r(&rows[i].created, &tm);
        strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M", &tm);
        for (char *p = rows[i].first; *p; p++) if (*p == '\n') *p = ' ';
        printf("  %d) [%lld] %s | %s\n", i+1, (long long)rows[i].id, tb,
               rows[i].first[0] ? rows[i].first : "(empty)");
    }
    printf("  n) new session\nselect: "); fflush(stdout);

    char buf[32];
    if (!fgets(buf, sizeof(buf), stdin)) { free(rows); return -1; }
    int64_t result;
    if (buf[0] == 'n' || buf[0] == 'N') result = session_create(db, "cli", NULL, -1, 0);
    else { int ch = atoi(buf); result = (ch >= 1 && ch <= count) ? rows[ch-1].id : -1; }
    free(rows);
    return result;
}

/* ── CLI turn trigger ───────────────────────────────────────────── */

static void cli_start_turn(const char *input) {
    inbox_insert(g_db, g_cli_session, "cli", input);
    inbox_consume_into_entries(g_db, g_cli_session, 100);
    g_cli_turn_active = 1;
    dispatch_llm_req(g_cli_session, g_agent_name, 0);
}

/* ── main ───────────────────────────────────────────────────────── */

/* Extract builtin extension templates to ~/.cclaw/extensions/ on first run */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
static void extract_builtin_extensions(sqlite3 *db, const char *db_path) {
    /* Check if any extensions are registered */
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM extensions", -1, &s, NULL) != SQLITE_OK) return;
    int has_ext = 0;
    if (sqlite3_step(s) == SQLITE_ROW) has_ext = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    if (has_ext > 0) return;

    /* Derive base dir from db_path (strip /cclaw.db) */
    char base[PATH_MAX];
    snprintf(base, sizeof(base), "%s", db_path);
    char *sl = strrchr(base, '/');
    if (sl) *sl = '\0'; else return;

    /* Create extensions/telegram/ directory */
    char tg_dir[2*PATH_MAX];
    snprintf(tg_dir, sizeof(tg_dir), "%s/extensions/telegram", base);
    char mkdir_cmd[2*PATH_MAX];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "%s/extensions/telegram/.keep", base);
    ensure_parent_dir(mkdir_cmd);

    /* Write channel.js */
    char js_path[2*PATH_MAX];
    snprintf(js_path, sizeof(js_path), "%s/channel.js", tg_dir);
    FILE *f = fopen(js_path, "w");
    if (f) { fputs(TPL_CHANNEL_TELEGRAM_JS, f); fclose(f); }

    /* Write telegram.json */
    char json_path[2*PATH_MAX];
    snprintf(json_path, sizeof(json_path), "%s/telegram.json", tg_dir);
    f = fopen(json_path, "w");
    if (f) { fputs(TPL_CHANNEL_TELEGRAM_JSON, f); fclose(f); }

    /* Register in extensions table */
    const char *isql = "INSERT OR IGNORE INTO extensions(name, path, builtin)"
                       " VALUES('telegram', ?, 1);";
    sqlite3_stmt *ins;
    if (sqlite3_prepare_v2(db, isql, -1, &ins, NULL) == SQLITE_OK) {
        sqlite3_bind_text(ins, 1, tg_dir, -1, SQLITE_STATIC);
        sqlite3_step(ins);
        sqlite3_finalize(ins);
    }
}

int main(int argc, char *argv[]) {
    cclaw_log_init();
    int daemon_mode = 0, llm_mode = 0, new_session = 0, yolo_mode = 0;
    const char *channel_mode = NULL;
    LogLevel log_level_override = LOG_LEVEL_INFO;
    int log_level_set = 0;
    int64_t session_id = -1;
    const char *prompt = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) { print_usage(); return 0; }
        else if (strcmp(argv[i], "--daemon") == 0) daemon_mode = 1;
        else if (strcmp(argv[i], "--channel") == 0) { if (++i >= argc) { fprintf(stderr, "--channel requires name\n"); return 1; } channel_mode = argv[i]; }
        else if (strcmp(argv[i], "llm") == 0) llm_mode = 1;
        else if (strncmp(argv[i], "--log-level=", 12) == 0) { log_level_override = log_level_parse(argv[i]+12); log_level_set = 1; }
        else if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-v") == 0) { log_level_override = LOG_LEVEL_DEBUG; log_level_set = 1; }
        else if (strcmp(argv[i], "--trace") == 0 || strcmp(argv[i], "-vv") == 0) { log_level_override = LOG_LEVEL_TRACE; log_level_set = 1; }
        else if (strcmp(argv[i], "--new") == 0) new_session = 1;
        else if (strcmp(argv[i], "-y") == 0) yolo_mode = 1;
        else if (strcmp(argv[i], "--llm-fork") == 0) g_llm_fork = 1;
        else if (strncmp(argv[i], "--llm-threads=", 14) == 0) g_llm_threads = atoi(argv[i]+14);
        else if (strcmp(argv[i], "-p") == 0) { if (++i >= argc) { fprintf(stderr, "-p requires arg\n"); return 1; } prompt = argv[i]; }
        else if (strcmp(argv[i], "-s") == 0) { if (++i >= argc) { fprintf(stderr, "-s requires arg\n"); return 1; } session_id = atoll(argv[i]); }
        else if (strncmp(argv[i], "--session-id=", 13) == 0) session_id = atoll(argv[i]+13);
        else { fprintf(stderr, "unknown option: %s\n", argv[i]); return 1; }
    }

    /* Env var overrides (CLI flags take precedence) */
    if (!g_llm_fork) {
        const char *v = getenv("CCLAW_LLM_FORK");
        if (v && v[0] == '1') g_llm_fork = 1;
    }
    {   const char *v = getenv("CCLAW_LLM_THREADS");
        if (v && atoi(v) > 0) g_llm_threads = atoi(v);
    }

    /* LLM subprocess mode — direct call, no poll */
    if (llm_mode) {
        if (session_id < 0) { fprintf(stderr, "llm requires -s <id>\n"); return 1; }
        return llm_proc_main(session_id);
    }

    shutdown_init();

    /* ── Open DB ─────────────────────────────────────────────────── */
    char *db_path = resolve_db_path();
    ensure_parent_dir(db_path);
    g_db = db_open(db_path);
    if (!g_db) { fprintf(stderr, "cannot open DB: %s\n", db_path); free(db_path); return 1; }
    if (db_ensure_schema(g_db) != 0) { db_close(g_db); free(db_path); return 1; }

    { uint8_t sk[32]; if (secret_key_load_or_create(db_path, sk) == 0) db_set_secret_key(sk); }

    setenv("CCLAW_DB", db_path, 1);

    /* First-run initialization (no-op if already seeded) */
    db_seed_defaults(g_db);
    extract_builtin_extensions(g_db, db_path);

    g_cfg = config_load(g_db);
    if (!g_cfg) { fprintf(stderr, "config load failed\n"); db_close(g_db); return 1; }
    if (log_level_set) {
        g_cfg->log_level = log_level_override;
        const char *lvl_str = log_level_override == LOG_LEVEL_TRACE ? "trace" :
                              log_level_override == LOG_LEVEL_DEBUG ? "debug" :
                              log_level_override == LOG_LEVEL_ERROR ? "error" : "info";
        setenv("CCLAW_LOG_LEVEL", lvl_str, 1);
    }

    /* Enable SQLite query profiling at trace level */
    if (g_cfg->log_level >= LOG_LEVEL_DEBUG)
        db_enable_trace(g_db);

    /* ── Daemon mode ─────────────────────────────────────────────── */
    /* ── Channel mode ─────────────────────────────────────────────── */
    if (channel_mode) {
        config_free(g_cfg); db_close(g_db);
        execl("build/channel_runner", "channel_runner", db_path, channel_mode, (char *)NULL);
        perror("execl channel_runner");
        free(db_path);
        return 1;
    }

    if (daemon_mode) {
        g_mode = 1;
        workspace_init(g_cfg);
        printf("cclaw %s — daemon mode\n", CCLAW_VERSION);
        web_start(g_cfg, g_db);
        heartbeat_start(g_cfg, g_db);
        cron_start(g_cfg, g_db);

        /* Startup recovery — reset stale sessions */
        {   const char *rsql = "UPDATE sessions SET state='idle' WHERE state='running';";
            sqlite3_exec(g_db, rsql, NULL, NULL, NULL); }

        /* Init wake pipe + FIFO */
        wake_init();
        int fifo_fd = wake_fifo_open(db_path);

        /* SIGCHLD self-pipe */
        if (pipe(g_chld_pipe) != 0) { perror("pipe"); return 1; }
        fcntl(g_chld_pipe[0], F_SETFD, FD_CLOEXEC);
        fcntl(g_chld_pipe[1], F_SETFD, FD_CLOEXEC);
        set_nonblock(g_chld_pipe[0]); set_nonblock(g_chld_pipe[1]);
        { struct sigaction sa = {0}; sa.sa_handler = sigchld_handler;
          sigemptyset(&sa.sa_mask); sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
          sigaction(SIGCHLD, &sa, NULL); }

        /* Launch channel processes */
        channel_launch_all(g_db, db_path);

        /* poll() setup */
        struct pollfd pfds[4];
        int nfds_total = 0;
        pfds[nfds_total].fd = g_chld_pipe[0]; pfds[nfds_total].events = POLLIN; nfds_total++;
        pfds[nfds_total].fd = wake_fd(); pfds[nfds_total].events = POLLIN; nfds_total++;
        int fifo_idx = -1;
        if (fifo_fd >= 0) { fifo_idx = nfds_total; pfds[nfds_total].fd = fifo_fd; pfds[nfds_total].events = POLLIN; nfds_total++; }

        /* Daemon event loop */
        while (!shutdown_requested()) {
            int rc = poll(pfds, (nfds_t)nfds_total, 1000);
            if (rc < 0) { if (errno == EINTR) continue; break; }

            if (pfds[0].revents & POLLIN) {
                char buf[64];
                while (read(g_chld_pipe[0], buf, sizeof(buf)) > 0) {}
                reap_children();
            }
            if (pfds[1].revents & POLLIN) {
                WakeMsg msg;
                while (read(wake_fd(), &msg, sizeof(msg)) == (ssize_t)sizeof(msg)) {
                    if (child_has_session(msg.session_id)) continue;
                    char *aname = session_get_agent_name(g_db, msg.session_id);
                    if (aname) { dispatch_llm_req(msg.session_id, aname, 0); free(aname); }
                }
            }
            if (fifo_idx >= 0 && (pfds[fifo_idx].revents & POLLIN)) {
                char drain[64];
                while (read(fifo_fd, drain, sizeof(drain)) > 0) {}
                channel_consume_events(g_db);
            }
            if (rc == 0 && g_child_count > 0) reap_children();
        }

        /* Shutdown */
        channel_shutdown_all();
        cron_stop(); heartbeat_stop(); web_stop();
        close(g_chld_pipe[0]); close(g_chld_pipe[1]);
        wake_close(); wake_fifo_close(fifo_fd, db_path);
        config_free(g_cfg); db_close(g_db); free(db_path);
        return 0;
    }

    /* ── CLI mode ────────────────────────────────────────────────── */
    g_mode = 0;

    if (!g_cfg->provider.api_key || !g_cfg->provider.api_key[0]) {
        fprintf(stderr, "error: no API key (set OPENROUTER_API_KEY)\n");
        config_free(g_cfg); db_close(g_db); free(db_path); return 1;
    }

    /* Derive base_dir for workspace */
    char *base_dir = strdup(db_path);
    { char *sl = strrchr(base_dir, '/'); if (sl) *sl = '\0'; else { free(base_dir); base_dir = strdup("."); } }

    /* Ensure default agent exists — bootstrap on first run */
    { int ac = 0; char **al = db_agent_list(g_db, &ac);
      if (!al || ac == 0) {
          char ws[PATH_MAX]; snprintf(ws, sizeof(ws), "%s/agents/default/workspace/.keep", base_dir);
          ensure_parent_dir(ws);
          /* Create bootstrap agent with elevated permissions */
          const char *bootstrap_sql =
              "INSERT OR IGNORE INTO agents(name, system_prompt, trust_level, allowed_tools)"
              " VALUES('default', ?, 'bootstrap', ?);"
              ;
          const char *bootstrap_tools = "[\"configure_provider\",\"configure_channel\",\"create_agent\",\"file_read\",\"file_write\",\"js_eval\",\"request_config\"]";
          sqlite3_stmt *bs;
          if (sqlite3_prepare_v2(g_db, bootstrap_sql, -1, &bs, NULL) == SQLITE_OK) {
              sqlite3_bind_text(bs, 1, TPL_BOOTSTRAP_SYSTEM_PROMPT_MD, -1, SQLITE_STATIC);
              sqlite3_bind_text(bs, 2, bootstrap_tools, -1, SQLITE_STATIC);
              sqlite3_step(bs); sqlite3_finalize(bs);
          }
          db_kv_set(g_db, "default_agent", "default");
          /* Seed default memory blocks */
          memory_block_create(g_db, "default", "AGENT",
              "Your identity, capabilities, and operational notes. Update as you learn about yourself.",
              NULL, 5000);
          memory_block_create(g_db, "default", "USER",
              "Information about the user: preferences, context, working style. Update as you learn.",
              NULL, 5000);
      }
      if (al) { for (int i = 0; i < ac; i++) free(al[i]); free(al); }
    }

    /* Agent selection */
    char *agent_sel = NULL;
    if (prompt || !isatty(STDIN_FILENO)) {
        char *def = db_kv_get(g_db, "default_agent");
        agent_sel = def ? def : strdup("default");
    } else {
        int ac = 0; char **al = db_agent_list(g_db, &ac);
        if (ac == 1) { agent_sel = strdup(al[0]); }
        else if (ac > 1) {
            printf("agents:\n");
            for (int i = 0; i < ac; i++) printf("  %d) %s\n", i+1, al[i]);
            printf("select: "); fflush(stdout);
            char buf[64]; if (fgets(buf, sizeof(buf), stdin)) {
                int ch = atoi(buf);
                if (ch >= 1 && ch <= ac) agent_sel = strdup(al[ch-1]);
            }
        }
        if (al) { for (int i = 0; i < ac; i++) free(al[i]); free(al); }
    }
    if (!agent_sel) { fprintf(stderr, "no agent selected\n"); free(base_dir); config_free(g_cfg); db_close(g_db); free(db_path); return 1; }
    snprintf(g_agent_name, sizeof(g_agent_name), "%s", agent_sel);
    setenv("CCLAW_AGENT_NAME", g_agent_name, 1);
    free(agent_sel);

    /* Inject agent config env vars */
    if (!yolo_mode) {
        AgentConfig *ac = agent_config_load_db(g_db, g_agent_name);
        if (ac) {
            if (ac->tool_count > 0) {
                size_t len = 0; for (size_t i = 0; i < ac->tool_count; i++) len += strlen(ac->tools[i]) + 1;
                char *csv = malloc(len); if (csv) { csv[0] = '\0';
                    for (size_t i = 0; i < ac->tool_count; i++) { if (i) strcat(csv, ","); strcat(csv, ac->tools[i]); }
                    setenv("CCLAW_TOOLS", csv, 1); free(csv); }
            }
            if (ac->allowed_hosts_count > 0) {
                size_t len = 0; for (size_t i = 0; i < ac->allowed_hosts_count; i++) len += strlen(ac->allowed_hosts[i]) + 1;
                char *csv = malloc(len); if (csv) { csv[0] = '\0';
                    for (size_t i = 0; i < ac->allowed_hosts_count; i++) { if (i) strcat(csv, ","); strcat(csv, ac->allowed_hosts[i]); }
                    setenv("CCLAW_ALLOWED_HOSTS", csv, 1); free(csv); }
            }
            agent_config_free(ac);
        }
    }
    if (yolo_mode) setenv("CCLAW_YOLO", "1", 1);
    if (!getenv("CCLAW_STREAM")) { setenv("CCLAW_STREAM", "1", 1); g_cfg->stream = 1; }
    setenv("CCLAW_MODE", "cli", 1);
    { char cwd[PATH_MAX]; if (getcwd(cwd, sizeof(cwd))) setenv("CCLAW_PATH", cwd, 1); }
    workspace_init(g_cfg);

    /* Set up tool schemas env for LLM proc children */
    AgentSetup setup;
    agent_setup_init(&setup, g_db, 0, g_cfg, g_agent_name, NULL, 0, AGENT_SETUP_CLI);
    g_tool_setup = &setup;
    /* Set agents_dir for rename support; point agent_name at g_agent_name for live update */
    char agents_dir[PATH_MAX];
    snprintf(agents_dir, sizeof(agents_dir), "%s/agents", base_dir);
    setup.req_cfg_ctx.agents_dir = agents_dir;
    setup.req_cfg_ctx.agent_name = g_agent_name;

    /* Session selection */
    session_id = cli_select_session(g_db, session_id, new_session);
    if (session_id < 0) { agent_setup_destroy(&setup); free(base_dir); config_free(g_cfg); db_close(g_db); free(db_path); return 1; }
    g_cli_session = session_id;
    setup.req_cfg_ctx.session_id = session_id;

    /* ── SIGCHLD self-pipe ───────────────────────────────────────── */
    if (pipe(g_chld_pipe) != 0) { perror("pipe"); return 1; }
    fcntl(g_chld_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(g_chld_pipe[1], F_SETFD, FD_CLOEXEC);
    set_nonblock(g_chld_pipe[0]);
    set_nonblock(g_chld_pipe[1]);
    { struct sigaction sa = {0}; sa.sa_handler = sigchld_handler;
      sigemptyset(&sa.sa_mask); sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
      sigaction(SIGCHLD, &sa, NULL); }

    /* ── poll() setup ──────────────────────────────────────────────── */
    struct pollfd cli_pfds[4];
    int cli_nfds = 0;

    /* Start LLM worker (unless --llm-fork) */
    int worker_fd = -1;
    int worker_idx = -1;
    if (!g_llm_fork) {
        if (llm_worker_start(db_path, g_llm_threads) == 0) {
            worker_fd = llm_worker_fd();
            set_nonblock(worker_fd);
            worker_idx = cli_nfds;
            cli_pfds[cli_nfds].fd = worker_fd; cli_pfds[cli_nfds].events = POLLIN; cli_nfds++;
        } else {
            g_llm_fork = 1;
        }
    }

    /* Register SIGCHLD pipe */
    int chld_idx = cli_nfds;
    cli_pfds[cli_nfds].fd = g_chld_pipe[0]; cli_pfds[cli_nfds].events = POLLIN; cli_nfds++;

    /* Register stdin for interactive mode */
    int stdin_idx = -1;
    if (!prompt && isatty(STDIN_FILENO)) {
        set_nonblock(STDIN_FILENO);
        stdin_idx = cli_nfds;
        cli_pfds[cli_nfds].fd = STDIN_FILENO; cli_pfds[cli_nfds].events = POLLIN; cli_nfds++;
        printf("cclaw cli (type 'exit' or Ctrl-D to quit)\n> ");
        fflush(stdout);
    }

    /* Single-turn mode: -p <prompt> */
    if (prompt) {
        g_cli_done = 1;
        cli_start_turn(prompt);
    }

    /* ── Event loop ──────────────────────────────────────────────── */
    int rc = 0;

    while (!shutdown_requested()) {
        int nready = poll(cli_pfds, (nfds_t)cli_nfds, 500);
        if (nready < 0) { if (errno == EINTR) continue; break; }

        if (cli_pfds[chld_idx].revents & POLLIN) {
            char buf[64];
            while (read(g_chld_pipe[0], buf, sizeof(buf)) > 0) {}
            reap_children();

            if (g_mode == 0 && !g_cli_turn_active) {
                if (g_cli_done) goto done;
                printf("> "); fflush(stdout);
            }
        }
        if (worker_idx >= 0 && (cli_pfds[worker_idx].revents & POLLIN)) {
            int64_t completed_sid;
            while (llm_worker_read(&completed_sid) == 0) {
                if (completed_sid == -1) {
                    LOG_ERROR_(g_cfg, "LLM worker queue full");
                    continue;
                }
                ChildProc *wc = NULL;
                for (int j = 0; j < g_child_count; j++) {
                    if (g_children[j].session_id == completed_sid &&
                        g_children[j].type == CHILD_LLM_REQ &&
                        g_children[j].pid == -1) {
                        wc = &g_children[j];
                        break;
                    }
                }
                char aname[64] = "default";
                int iter = 0;
                if (wc) {
                    snprintf(aname, sizeof(aname), "%s", wc->agent_name);
                    iter = wc->iteration;
                    child_remove(wc);
                }
                handle_llm_complete(completed_sid, aname, iter);

                if (g_mode == 0 && !g_cli_turn_active) {
                    if (g_cli_done) goto done;
                    printf("> "); fflush(stdout);
                }
            }
        }
        if (stdin_idx >= 0 && (cli_pfds[stdin_idx].revents & POLLIN)) {
            if (g_cli_turn_active) continue;

            char *line = NULL; size_t cap = 0;
            ssize_t len = getline(&line, &cap, stdin);
            if (len < 0) { free(line); goto done; }
            if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';

            if (!line[0]) { free(line); printf("> "); fflush(stdout); continue; }
            if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) { free(line); goto done; }

            cli_start_turn(line);
            free(line);
        }

        if (nready == 0 && g_child_count > 0) reap_children();
    }

done:
    /* Print session cost */
    { int64_t cost = session_cost(g_db, g_cli_session);
      if (cost > 0) fprintf(stderr, "\n[session cost: $%.6f]\n", (double)cost / 1e9); }

    session_set_state(g_db, g_cli_session, "idle");
    agent_setup_destroy(&setup);
    llm_worker_stop();
    close(g_chld_pipe[0]); close(g_chld_pipe[1]);
    free(base_dir); config_free(g_cfg); db_close(g_db); free(db_path);
    return rc;
}
