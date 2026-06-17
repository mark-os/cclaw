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
#include "sandbox.h"
#include "tool_js.h"
#include <mquickjs.h>
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
#include "advance.h"
#include "channel.h"
#include "channel_api.h"
#include "secret.h"
#include "secret_scan.h"
#include "secret_interp.h"
#include "web.h"
#include "heartbeat.h"
#include "cron.h"

/* ── Constants ──────────────────────────────────────────────────── */

#define CHILD_MAX 48
#define TOOL_MAX_OUTPUT (60 * 1024)  /* 60KB — fits in pipe buffer */
#define DEFAULT_MAX_ITERATIONS 25

/* ── Child types and tracking ───────────────────────────────────── */

typedef enum { CHILD_CHANNEL, CHILD_TOOL_EXEC } ChildType;

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
    char *outbuf;           /* Accumulator for tool output (grows with realloc) */
    size_t outbuf_len;      /* Bytes currently in outbuf */
    /* Channel fields */
    char channel_name[64];
    char binary_path[512];
    int restart_count;
    /* Deadline: 0 = no timeout, >0 = SIGKILL after this time */
    time_t deadline;
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
    /* Clean up pipe and buffer before removal */
    if (c->result_pipe >= 0) {
        close(c->result_pipe);
        c->result_pipe = -1;
    }
    free(c->outbuf);
    c->outbuf = NULL;
    c->outbuf_len = 0;
    g_children[idx] = g_children[g_child_count - 1];
    g_child_count--;
}

static int child_has_session(int64_t session_id) {
    for (int i = 0; i < g_child_count; i++)
        if (g_children[i].type == CHILD_TOOL_EXEC
            && g_children[i].session_id == session_id)
            return 1;
    return 0;
}

/* ── Globals ────────────────────────────────────────────────────── */

/* Forward declarations */
static void deliver_response(int64_t session_id);
static int fork_tool_exec(int64_t session_id, const char *agent_name, PendingToolCall *tc);

static sqlite3 *g_db;
static Config *g_cfg;
static int g_mode;  /* 0=cli, 1=daemon */
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

/* ── dispatch_llm_req ────────────────────────────────────────────── */

/* Dispatch LLM request via worker thread pool */
static int dispatch_llm_req(int64_t session_id, const char *agent_name, int iteration) {
    if (child_has_session(session_id)) return -1;

    /* Turn start: move queued inbox into entries */
    if (iteration == 0)
        inbox_consume_into_entries(g_db, session_id, 100);

    if (!llm_worker_alive()) return -1;

    int max_iter = g_cfg->max_iterations > 0 ? g_cfg->max_iterations : DEFAULT_MAX_ITERATIONS;
    if (iteration >= max_iter) {
        int64_t turn_id = db_next_turn_id(g_db, session_id);
        Message msg = {.role = ROLE_ASSISTANT,
                       .content = "error: max iterations reached",
                       .stop_reason = STOP_REASON_ERROR};
        entry_append_with_turn(g_db, session_id, &msg, turn_id);
        session_set_state(g_db, session_id, "idle");
        if (g_mode == 0) {
            fprintf(stderr, "\nerror: max iterations reached\n");
            g_cli_turn_active = 0;
        }
        return -1;
    }

    /* Rate limit check */
    if (g_cfg->token_rate_limit > 0 && !rate_limit_check(g_db, NULL)) {
        session_set_state(g_db, session_id, "rate_limited");
        return -1;
    }

    session_set_state(g_db, session_id, "llm_running");
    return llm_worker_submit(g_db, session_id, agent_name, iteration == 0 ? 1 : 0);
}

/* ── fork_tool_exec ─────────────────────────────────────────────── */

/* Tools that run in-process (need parent's DB handle or user interaction) */
static int tool_is_inline(const char *name) {
    return strcmp(name, "request_config") == 0 ||
           strcmp(name, "memory_create") == 0 ||
           strcmp(name, "memory_append") == 0 ||
           strcmp(name, "memory_replace") == 0 ||
           strcmp(name, "db_query") == 0 ||
           strcmp(name, "launch_agent") == 0 ||
           strcmp(name, "check_session") == 0;
}

/* Tools that need {{SECRET:X}} resolved to real values at exec time */
static int tool_needs_interpolation(const char *name) {
    return strcmp(name, "shell_exec") == 0 ||
           strcmp(name, "web_fetch") == 0 ||
           strcmp(name, "js_eval") == 0;
}

static AgentSetup *g_tool_setup;  /* Initialized once for tool dispatch */

/* CLI progress: "[tool_name {"arg":"value"}]" dimmed, args truncated */
static void cli_print_tool_call(const char *name, const char *args) {
    fprintf(stdout, "\n\033[2m[%s", name);
    if (args && args[0] && strcmp(args, "{}") != 0) {
        if (strlen(args) <= 200) fprintf(stdout, " %s", args);
        else fprintf(stdout, " %.197s...", args);
    }
    fprintf(stdout, "]\033[0m ");
    fflush(stdout);
}

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
        /* Interpolate {{SECRET:X}} only for tools that exec with credentials */
        char *interp_args = NULL;
        if (tool_needs_interpolation(tc->name) && g_tool_setup && g_tool_setup->secret_count > 0)
            interp_args = secret_interpolate(tc->arguments, g_tool_setup->secrets, g_tool_setup->secret_count);
        /* Thread tool_call_id for launch_agent blocking mode */
        if (strcmp(tc->name, "launch_agent") == 0 && te->user_data)
            ((AgentLaunchCtx *)te->user_data)->current_tool_call_id = tc->call_id;
        char *result = te->handler(interp_args ? interp_args : tc->arguments, te->user_data);
        free(interp_args);
        /* NULL return means blocking: launch_agent parked the parent in
         * 'waiting' and left this tool_call pending (see tool_agent.c). Leave it
         * pending — advance_session writes the result on child completion. This
         * is an explicit allowlist on purpose: a NULL from any other tool must
         * fall through to "error: tool returned null" below, not silently hang. */
        if (!result && strcmp(tc->name, "launch_agent") == 0)
            return 2; /* Signal: parked, don't advance */
        if (!result) result = strdup("error: tool returned null");

        /* Postprocess: deinterpolate + secret scan (scan runs even with no
         * secrets loaded — inline js_eval output can carry leaked credentials) */
        { char *pp = tool_result_postprocess(result,
              g_tool_setup ? g_tool_setup->secrets : NULL,
              g_tool_setup ? g_tool_setup->secret_count : 0);
          if (pp) { free(result); result = pp; } }

        /* CLI progress */
        if (g_mode == 0) {
            cli_print_tool_call(tc->name, tc->arguments);
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
    if (g_mode == 0)
        cli_print_tool_call(tc->name, tc->arguments);

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
        /* Interpolate {{SECRET:X}} only for tools that exec with credentials */
        char *interp_args = NULL;
        if (tool_needs_interpolation(tc->name) && g_tool_setup && g_tool_setup->secret_count > 0)
            interp_args = secret_interpolate(tc->arguments, g_tool_setup->secrets, g_tool_setup->secret_count);
        char *result = te->handler(interp_args ? interp_args : tc->arguments, te->user_data);
        free(interp_args);
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
    /* Set read end nonblocking so we can drain as data arrives */
    set_nonblock(pipefd[0]);
    ChildProc *c = &g_children[g_child_count++];
    memset(c, 0, sizeof(*c));
    c->pid = pid;
    c->type = CHILD_TOOL_EXEC;
    c->session_id = session_id;
    c->turn_id = tc->turn_id;
    c->entry_id = tc->entry_id;
    c->result_pipe = pipefd[0];
    c->outbuf = NULL;
    c->outbuf_len = 0;
    c->deadline = time(NULL) + 120;
    snprintf(c->agent_name, sizeof(c->agent_name), "%s", agent_name);
    snprintf(c->tool_call_id, sizeof(c->tool_call_id), "%s", tc->call_id);
    return 0;
}

/* ── Pipe draining helpers ─────────────────────────────────────── */

/* Drain a tool child's result pipe (nonblocking) into c->outbuf, kept
 * NUL-terminated. Bytes beyond TOOL_MAX_OUTPUT are read and discarded so
 * the child never blocks on a full pipe. Closes the fd on EOF or error;
 * leaves it open on EAGAIN (more data may come). */
static void child_drain_pipe(ChildProc *c) {
    if (c->result_pipe < 0) return;

    char buf[4096];
    ssize_t n;
    while ((n = read(c->result_pipe, buf, sizeof(buf))) > 0) {
        size_t to_copy = (size_t)n;
        if (c->outbuf_len + to_copy > TOOL_MAX_OUTPUT)
            to_copy = TOOL_MAX_OUTPUT - c->outbuf_len;
        if (to_copy == 0) continue; /* at cap: keep draining, discard */
        char *tmp = realloc(c->outbuf, c->outbuf_len + to_copy + 1);
        if (!tmp) continue; /* OOM: drop chunk, keep child unblocked */
        memcpy(tmp + c->outbuf_len, buf, to_copy);
        c->outbuf = tmp;
        c->outbuf_len += to_copy;
        c->outbuf[c->outbuf_len] = '\0';
    }
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        close(c->result_pipe);
        c->result_pipe = -1;
    }
}

/* Append every live tool result pipe to a pollfd set being rebuilt. */
static int add_result_pipe_fds(struct pollfd *pfds, int nfds, int max) {
    for (int i = 0; i < g_child_count && nfds < max; i++) {
        if (g_children[i].type == CHILD_TOOL_EXEC && g_children[i].result_pipe >= 0) {
            pfds[nfds].fd = g_children[i].result_pipe;
            pfds[nfds].events = POLLIN;
            nfds++;
        }
    }
    return nfds;
}

/* Drain whichever result pipes poll() reported readable. */
static void drain_ready_result_pipes(const struct pollfd *pfds, int base, int nfds) {
    for (int i = base; i < nfds; i++) {
        if (!(pfds[i].revents & (POLLIN | POLLHUP))) continue;
        for (int j = 0; j < g_child_count; j++) {
            if (g_children[j].type == CHILD_TOOL_EXEC &&
                g_children[j].result_pipe == pfds[i].fd) {
                child_drain_pipe(&g_children[j]);
                break;
            }
        }
    }
}

/* ── reap_children (state machine) ──────────────────────────────── */

/* ── child_sweep_deadlines: kill timed-out children ──────────── */

static void child_sweep_deadlines(void) {
    time_t now = time(NULL);
    for (int i = 0; i < g_child_count; i++) {
        ChildProc *c = &g_children[i];
        if (c->deadline == 0 || c->pid <= 0 || now < c->deadline)
            continue;
        kill(c->pid, SIGKILL);
        if (c->type == CHILD_TOOL_EXEC) {
            if (c->result_pipe >= 0) { close(c->result_pipe); c->result_pipe = -1; }
            free(c->outbuf); c->outbuf = NULL; c->outbuf_len = 0;
            const char *err = "error: tool timed out (120s)";
            ToolResult tr = {.tool_call_id = c->tool_call_id, .content = (char *)err};
            Message msg = {.role = ROLE_TOOL, .tool_result = &tr,
                           .tool_name = "", .is_error = 1};
            entry_append_with_turn(g_db, c->session_id, &msg, c->turn_id);
            db_tool_call_complete_with_result(g_db, c->entry_id, c->tool_call_id, -1);
        }
        c->deadline = -1; /* mark consumed so reap doesn't double-advance */
    }
}

/* ── run_advance: call advance_session and execute the decision ── */

/* ── event_step: shared event handling for both daemon and CLI loops ── */

static void reap_children(void);
static void run_advance(int64_t session_id);

static void event_step_worker(int worker_fd) {
    (void)worker_fd;
    int64_t completed_sid;
    while (llm_worker_read(&completed_sid) == 0) {
        if (completed_sid == -1) continue;
        run_advance(completed_sid);
    }
}

static void event_step_chld(void) {
    char buf[64];
    while (read(g_chld_pipe[0], buf, sizeof(buf)) > 0) {}
    reap_children();
}

/* ── run_advance: call advance_session and execute the decision ── */

static void run_advance(int64_t session_id) {
    int max_iter = g_cfg->max_iterations > 0 ? g_cfg->max_iterations : DEFAULT_MAX_ITERATIONS;
    AdvanceOutput out = advance_session(g_db, session_id, max_iter);

    switch (out.action) {
    case ADVANCE_DISPATCH_LLM:
        if (dispatch_llm_req(session_id, out.agent_name, out.iteration) < 0) {
            if (g_mode == 0) g_cli_turn_active = 0;
        }
        break;
    case ADVANCE_DISPATCH_TOOLS: {
        int i = 0;
        int parked = 0;
        while (i < out.tc_count) {
            int rc = fork_tool_exec(session_id, out.agent_name, &out.calls[i]);
            if (rc == 1) { i++; continue; } /* inline tool — try next */
            if (rc == 2) { parked = 1; break; } /* blocking sub-agent */
            break; /* forked — stop (one at a time for forked) */
        }
        if (!parked && i >= out.tc_count) {
            /* All were inline — advance again (tools all done) */
            run_advance(session_id);
        }
        db_tool_call_free_pending(out.calls, out.tc_count);
        break;
    }
    case ADVANCE_DONE:
        deliver_response(session_id);
        /* Attempt compaction if configured */
        if (g_cfg->compaction && llm_worker_alive() &&
            session_needs_compaction(g_db, session_id, g_cfg)) {
            session_set_state(g_db, session_id, "compacting");
            if (llm_worker_submit_compact(g_db, session_id, out.agent_name) != 0)
                session_set_state(g_db, session_id, "idle");
        }
        if (g_mode == 0) {
            g_cli_turn_active = 0;
        }
        break;
    case ADVANCE_WAITING:
    case ADVANCE_NOOP:
        if (g_mode == 0 && !child_has_session(session_id))
            g_cli_turn_active = 0;
        break;
    case ADVANCE_ERROR:
        if (g_mode == 0) {
            fprintf(stderr, "error: session advance failed\n");
            g_cli_turn_active = 0;
        }
        break;
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
        ChildProc *c = child_find(pid);
        if (!c) {
            /* Check if it's a channel process */
            channel_reap(pid, g_db);
            continue;
        }

        if (c->type == CHILD_TOOL_EXEC) {
            int64_t session_id = c->session_id;

            /* If killed by deadline sweep, result already written */
            if (c->deadline == -1) {
                child_remove(c);
                run_advance(session_id);
                continue;
            }

            /* Crash detection: signal or nonzero exit → synthesize error */
            int crashed = WIFSIGNALED(status) ||
                          (WIFEXITED(status) && WEXITSTATUS(status) != 0);

            /* Drain what's left */
            child_drain_pipe(c);
            if (c->result_pipe >= 0) { close(c->result_pipe); c->result_pipe = -1; }

            char *output;
            if (crashed && (!c->outbuf || c->outbuf_len == 0)) {
                /* No output and child crashed — synthesize error */
                char err[128];
                if (WIFSIGNALED(status))
                    snprintf(err, sizeof(err), "error: tool killed by signal %d", WTERMSIG(status));
                else
                    snprintf(err, sizeof(err), "error: tool exited %d", WEXITSTATUS(status));
                output = strdup(err);
            } else if (c->outbuf) {
                output = c->outbuf;
            } else {
                output = strdup("");
            }
            size_t out_len = output ? strlen(output) : 0;
            if (!output) output = strdup("error: OOM");

            /* Secret postprocess: deinterpolate + scan */
            { char *pp = tool_result_postprocess(output,
                  g_tool_setup ? g_tool_setup->secrets : NULL,
                  g_tool_setup ? g_tool_setup->secret_count : 0);
              if (pp) { free(output); output = pp; out_len = strlen(output); } }

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
            c->outbuf = NULL;
            c->outbuf_len = 0;

            child_remove(c);
            run_advance(session_id);
        }
    }
}

/* ── Helpers ────────────────────────────────────────────────────── */

static void ensure_parent_dir(const char *path) {
    char *dup = strdup(path);
    if (!dup) return;
    char *slash = strrchr(dup, '/');
    if (slash && slash != dup) {
        *slash = '\0';
        /* mkdir(2) each component — no shell, no injection */
        for (char *p = dup + 1; ; p++) {
            if (*p == '/' || *p == '\0') {
                char saved = *p;
                *p = '\0';
                if (mkdir(dup, 0755) != 0 && errno != EEXIST) break;
                *p = saved;
                if (saved == '\0') break;
            }
        }
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
           "  --mjs_eval         sandboxed JS evaluator (forked child mode)\n"
           "\n"
           "options:\n"
           "  -p <prompt>        single-turn: send prompt, print response, exit\n"
           "  -s <id>            session id\n"
           "  -y                 host mode: no sandbox, all tools and hosts allowed\n"
           "  --new              create a new session\n"
           "  --log-level=LEVEL  set log level (error|info|debug|trace)\n"
           "  -v, --debug        debug logging (timing, context stats)\n"
           "  -vv, --trace       trace logging (full req/resp JSON)\n"
           "  --help             show this help\n");
}

/* Session picker (preserved from old main.c) */
static int64_t cli_select_session(sqlite3 *db, int64_t requested_id, int new_session) {
    if (new_session) return session_create(db, "cli", g_agent_name, -1, 0);
    if (requested_id > 0) return requested_id;

    const char *sql =
        "SELECT s.id, s.created_at,"
        " (SELECT substr(e.content,1,50) FROM entries e WHERE e.session_id=s.id AND e.role=1 ORDER BY e.id ASC LIMIT 1),"
        " (SELECT substr(e.content,1,50) FROM entries e WHERE e.session_id=s.id AND e.role=1 ORDER BY e.id DESC LIMIT 1)"
        " FROM sessions s ORDER BY s.updated_at DESC;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return session_create(db, "cli", g_agent_name, -1, 0);

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

    if (count == 0) { free(rows); return session_create(db, "cli", g_agent_name, -1, 0); }
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
    if (buf[0] == 'n' || buf[0] == 'N') result = session_create(db, "cli", g_agent_name, -1, 0);
    else { int ch = atoi(buf); result = (ch >= 1 && ch <= count) ? rows[ch-1].id : -1; }
    free(rows);
    return result;
}

/* ── CLI turn trigger ───────────────────────────────────────────── */

static void cli_start_turn(const char *input) {
    inbox_insert_scanned(g_db, g_cli_session, "cli", input);
    g_cli_turn_active = 1;
    run_advance(g_cli_session);
}

/* ── main ───────────────────────────────────────────────────────── */

/* Extract builtin extension templates to ~/.cclaw/extensions/ on first run */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
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

/* ── --mjs_eval mode: sandboxed one-shot JS evaluator ────────────── */

extern const JSSTDLibraryDef js_std_library_eval;

#define MJS_HEAP_SIZE (1024 * 1024)
#define MJS_MAX_INSTRUCTIONS 10000000
#define MJS_MAX_OUTPUT (60 * 1024)
#define MJS_MAX_FILE  (1024 * 1024)

static const char *MJS_EVAL_PRELUDE =
    "var __console_buf = [];\n"
    "var console = {\n"
    "  log: function() {\n"
    "    var parts = [];\n"
    "    for (var i = 0; i < arguments.length; i++) {\n"
    "      var v = arguments[i];\n"
    "      parts.push(typeof v === 'object' ? JSON.stringify(v) : '' + v);\n"
    "    }\n"
    "    __console_buf.push(parts.join(' '));\n"
    "  }\n"
    "};\n"
    "console.warn = console.log;\n"
    "console.error = console.log;\n"
    "var require = function() {\n"
    "  throw new TypeError('require() not available. Use fs.readFile() for files, http_fetch() for network.');\n"
    "};\n"
    "var process = {};\n"
    "Object.defineProperty(process, 'env', {get: function() { throw new TypeError('process.env not available.'); }});\n"
    "Object.defineProperty(process, 'cwd', {get: function() { throw new TypeError('process.cwd not available. Use fs.cwd().'); }});\n"
    "Object.defineProperty(process, 'argv', {get: function() { throw new TypeError('process.argv not available.'); }});\n"
    "Object.defineProperty(process, 'exit', {get: function() { throw new TypeError('process.exit not available.'); }});\n"
    "Object.defineProperty(process, 'platform', {get: function() { throw new TypeError('process.platform not available.'); }});\n"
    "var module = {};\n"
    "Object.defineProperty(module, 'exports', {\n"
    "  get: function() { throw new TypeError('module.exports not available.'); },\n"
    "  set: function() { throw new TypeError('module.exports not available. Return your value as the last expression.'); }\n"
    "});\n"
    "var Map = function() { throw new TypeError('Map not available. Use plain objects.'); };\n"
    "var Set = function() { throw new TypeError('Set not available. Use: var s = {}; s[x] = true;'); };\n";

static int mjs_interrupt_handler(JSContext *ctx, void *opaque) {
    (void)ctx;
    JsHostCtx *hctx = (JsHostCtx *)opaque;
    hctx->instruction_count++;
    return hctx->instruction_count > hctx->instruction_limit;
}

static int mjs_eval_main(int argc, char **argv) {
    /* (a) Parse argv after "--mjs_eval" */
    int inline_mode = 0;
    const char *code_str = NULL;
    const char *file_path = NULL;
    const char *args_json = NULL;

    if (argc < 3) {
        fprintf(stderr, "usage: cclaw --mjs_eval -e 'CODE' | FILE [ARGS_JSON]\n");
        _exit(1);
    }
    if (strcmp(argv[2], "-e") == 0) {
        if (argc < 4) { fprintf(stderr, "--mjs_eval -e requires code\n"); _exit(1); }
        inline_mode = 1;
        code_str = argv[3];
    } else {
        file_path = argv[2];
        if (argc >= 4) args_json = argv[3];
    }

    /* (b) Read env vars into locals before sandbox scrubs them */
    const char *workspace_env = getenv("CCLAW_WORKSPACE");
    char *workspace = workspace_env ? strdup(workspace_env) : NULL;
    const char *db_env = getenv("CCLAW_DB");
    char *db_path = db_env ? strdup(db_env) : NULL;
    const char *proxy_env = getenv("CCLAW_PROXY_SOCK");
    char *proxy_sock = proxy_env ? strdup(proxy_env) : NULL;
    const char *host_mode_env = getenv("CCLAW_MJS_HOST");
    int no_sandbox = (host_mode_env && strcmp(host_mode_env, "1") == 0);

    /* Parse allowed hosts */
    char **allowed_hosts = NULL;
    size_t hosts_count = 0;
    const char *hosts_env = getenv("CCLAW_ALLOWED_HOSTS");
    if (hosts_env && hosts_env[0]) {
        char *tmp = strdup(hosts_env);
        char *tok = strtok(tmp, ",");
        while (tok) { hosts_count++; tok = strtok(NULL, ","); }
        allowed_hosts = malloc(hosts_count * sizeof(char *));
        /* re-parse */
        free(tmp);
        tmp = strdup(hosts_env);
        tok = strtok(tmp, ",");
        for (size_t i = 0; i < hosts_count; i++) {
            allowed_hosts[i] = strdup(tok);
            tok = strtok(NULL, ",");
        }
        free(tmp);
    }

    /* rlimits */
    const char *rl_nproc = getenv("CCLAW_RLIMIT_NPROC");
    const char *rl_as = getenv("CCLAW_RLIMIT_AS_MB");
    const char *rl_cpu = getenv("CCLAW_RLIMIT_CPU");

    /* (c) Sandbox */
    SandboxConfig cfg = {0};
    cfg.workspace = workspace;
    cfg.db_path = db_path;
    cfg.cwd_path = NULL;
    cfg.proxy_sock = proxy_sock;
    cfg.env_mode = 1;
    cfg.mount_cwd = 0;
    cfg.workspace_ro = 0;
    cfg.net_mode = (hosts_count > 0) ? 0 : 1;
    cfg.sandbox = no_sandbox ? 0 : 1;
    cfg.rlimits.nproc = rl_nproc ? atoi(rl_nproc) : 0;
    cfg.rlimits.as_mb = rl_as ? atoi(rl_as) : 0;
    cfg.rlimits.cpu_sec = rl_cpu ? atoi(rl_cpu) : 0;

    if (sandbox_child_setup(&cfg) != 0) {
        printf("error: sandbox setup failed\n");
        _exit(126);
    }

    /* (d) Create JS context with eval profile */
    void *heap = malloc(MJS_HEAP_SIZE);
    if (!heap) { printf("error: out of memory\n"); _exit(1); }

    JSContext *ctx = JS_NewContext(heap, MJS_HEAP_SIZE, &js_std_library_eval);
    if (!ctx) { free(heap); printf("error: JS context creation failed\n"); _exit(1); }

    JsHostCtx hctx = {
        .instruction_count = 0,
        .instruction_limit = MJS_MAX_INSTRUCTIONS,
        .allowed_hosts = allowed_hosts,
        .allowed_hosts_count = hosts_count,
        .tool_registry = NULL,
        .call_depth = 0
    };
    JS_SetInterruptHandler(ctx, mjs_interrupt_handler);
    JS_SetContextOpaque(ctx, &hctx);

    /* (e) Run prelude */
    JSValue pv = JS_Eval(ctx, MJS_EVAL_PRELUDE, strlen(MJS_EVAL_PRELUDE), "<prelude>", 0);
    if (JS_IsException(pv)) {
        JSValue exc = JS_GetException(ctx);
        JSCStringBuf buf;
        const char *msg = JS_ToCString(ctx, exc, &buf);
        printf("error: prelude failed: %s\n", msg ? msg : "unknown");
        _exit(1);
    }

    /* (f) Build code to eval */
    char *eval_code = NULL;
    if (inline_mode) {
        eval_code = (char *)code_str;  /* no free needed */
    } else {
        /* Read file (after sandbox, so sandboxed fs view) */
        FILE *f = fopen(file_path, "r");
        if (!f) { printf("error: cannot open %s\n", file_path); _exit(1); }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        if (sz < 0 || sz > MJS_MAX_FILE) {
            fclose(f);
            printf("error: file too large or unreadable\n");
            _exit(1);
        }
        fseek(f, 0, SEEK_SET);
        char *fbuf = malloc((size_t)sz + 1);
        if (!fbuf) { fclose(f); printf("error: out of memory\n"); _exit(1); }
        size_t rd = fread(fbuf, 1, (size_t)sz, f);
        fclose(f);
        fbuf[rd] = '\0';

        if (args_json) {
            /* Wrap: (function(args){\n<file>\n})(<args>) */
            size_t wlen = 20 + rd + 4 + strlen(args_json) + 2;
            eval_code = malloc(wlen);
            if (!eval_code) { printf("error: out of memory\n"); _exit(1); }
            snprintf(eval_code, wlen, "(function(args){\n%s\n})(%s)", fbuf, args_json);
            free(fbuf);
        } else {
            eval_code = fbuf;
        }
    }

    /* (g) Eval */
    size_t eval_len = strlen(eval_code);
    JSValue val = JS_Eval(ctx, eval_code, eval_len, "<mjs_eval>", JS_EVAL_RETVAL);

    int failed = 0;
    char *result = NULL;

    if (JS_IsException(val)) {
        failed = 1;
        JSValue exc = JS_GetException(ctx);
        JSCStringBuf buf;
        const char *msg = JS_ToCString(ctx, exc, &buf);
        if (msg) {
            size_t len = strlen(msg) + 16;
            result = malloc(len);
            if (result) snprintf(result, len, "error: %s", msg);
            else result = strdup("error: OOM");
        } else {
            result = strdup("error: exception (no message)");
        }
    } else if (hctx.instruction_count > MJS_MAX_INSTRUCTIONS) {
        failed = 1;
        result = strdup("error: instruction limit exceeded (10M)");
    } else if (JS_IsUndefined(val)) {
        /* (h) Console fallback */
        const char *check = "__console_buf.length > 0 ? __console_buf.join('\\n') : undefined";
        JSValue buf_val = JS_Eval(ctx, check, strlen(check), "<console>", JS_EVAL_RETVAL);
        if (!JS_IsUndefined(buf_val)) {
            JSCStringBuf sb;
            const char *str = JS_ToCString(ctx, buf_val, &sb);
            result = str ? strdup(str) : strdup("undefined");
        } else {
            result = strdup("undefined");
        }
    } else if (JS_IsNull(val)) {
        result = strdup("null");
    } else {
        JSCStringBuf buf;
        const char *str = JS_ToCString(ctx, val, &buf);
        result = str ? strdup(str) : strdup("error: cannot convert result to string");
    }

    /* (i) Output */
    size_t out_len = strlen(result);
    if (out_len > MJS_MAX_OUTPUT) out_len = MJS_MAX_OUTPUT;
    fwrite(result, 1, out_len, stdout);
    if (out_len > 0 && result[out_len - 1] != '\n') fwrite("\n", 1, 1, stdout);
    fflush(stdout);

    /* Cleanup */
    free(result);
    if (!inline_mode && eval_code != code_str) free(eval_code);
    JS_FreeContext(ctx);
    free(heap);
    for (size_t i = 0; i < hosts_count; i++) free(allowed_hosts[i]);
    free(allowed_hosts);
    free(workspace);
    free(db_path);
    free(proxy_sock);

    _exit(failed ? 1 : 0);
}

int main(int argc, char *argv[]) {
    /* --mjs_eval: early intercept before any config/logging setup */
    if (argc >= 2 && strcmp(argv[1], "--mjs_eval") == 0) return mjs_eval_main(argc, argv);

    cclaw_log_init();
    int daemon_mode = 0, new_session = 0, host_mode = 0;
    const char *channel_mode = NULL;
    LogLevel log_level_override = LOG_LEVEL_INFO;
    int log_level_set = 0;
    int64_t session_id = -1;
    const char *prompt = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) { print_usage(); return 0; }
        else if (strcmp(argv[i], "--daemon") == 0) daemon_mode = 1;
        else if (strcmp(argv[i], "--channel") == 0) { if (++i >= argc) { fprintf(stderr, "--channel requires name\n"); return 1; } channel_mode = argv[i]; }
        else if (strncmp(argv[i], "--log-level=", 12) == 0) { log_level_override = log_level_parse(argv[i]+12); log_level_set = 1; }
        else if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-v") == 0) { log_level_override = LOG_LEVEL_DEBUG; log_level_set = 1; }
        else if (strcmp(argv[i], "--trace") == 0 || strcmp(argv[i], "-vv") == 0) { log_level_override = LOG_LEVEL_TRACE; log_level_set = 1; }
        else if (strcmp(argv[i], "--new") == 0) new_session = 1;
        else if (strcmp(argv[i], "-y") == 0) host_mode = 1;
        else if (strcmp(argv[i], "--no-stream") == 0) setenv("CCLAW_STREAM", "0", 1);
        else if (strncmp(argv[i], "--llm-threads=", 14) == 0) g_llm_threads = atoi(argv[i]+14);
        else if (strcmp(argv[i], "-p") == 0) { if (++i >= argc) { fprintf(stderr, "-p requires arg\n"); return 1; } prompt = argv[i]; }
        else if (strcmp(argv[i], "-s") == 0) { if (++i >= argc) { fprintf(stderr, "-s requires arg\n"); return 1; } session_id = atoll(argv[i]); }
        else if (strncmp(argv[i], "--session-id=", 13) == 0) session_id = atoll(argv[i]+13);
        else { fprintf(stderr, "unknown option: %s\n", argv[i]); return 1; }
    }

    {   const char *v = getenv("CCLAW_LLM_THREADS");
        if (v && atoi(v) > 0) g_llm_threads = atoi(v);
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
    cclaw_log_set_level(g_cfg->log_level);

    /* Enable SQLite query profiling at trace level */
    if (g_cfg->log_level >= LOG_LEVEL_DEBUG)
        db_enable_trace(g_db);

    /* ── Channel mode ─────────────────────────────────────────────── */
    if (channel_mode) {
        config_free(g_cfg); db_close(g_db);
        /* channel_runner lives next to the cclaw binary (dev: build/) or in
         * ../lib/cclaw/ relative to it (prod: /usr/local/lib/cclaw/). Resolve
         * from /proc/self/exe so the daemon's cwd doesn't matter. */
        char self[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n > 0) {
            self[n] = '\0';
            char *slash = strrchr(self, '/');
            if (slash) {
                *slash = '\0';
                char runner[PATH_MAX];
                snprintf(runner, sizeof(runner), "%s/channel_runner", self);
                execl(runner, "channel_runner", db_path, channel_mode, (char *)NULL);
                snprintf(runner, sizeof(runner), "%s/../lib/cclaw/channel_runner", self);
                execl(runner, "channel_runner", db_path, channel_mode, (char *)NULL);
            }
        }
        perror("execl channel_runner");
        free(db_path);
        return 1;
    }

    /* ── Daemon mode ─────────────────────────────────────────────── */

    if (daemon_mode) {
        g_mode = 1;
        workspace_init(g_cfg);
        printf("cclaw %s — daemon mode\n", CCLAW_VERSION);
        web_start(g_cfg, g_db, db_path);
        heartbeat_start(g_cfg, g_db);
        cron_start(g_cfg, g_db);

        /* Start LLM worker threads */
        if (llm_worker_start(db_path, g_llm_threads) != 0) {
            fprintf(stderr, "error: failed to start LLM worker\n");
            config_free(g_cfg); db_close(g_db); free(db_path); return 1;
        }
        int daemon_worker_fd = llm_worker_fd();
        set_nonblock(daemon_worker_fd);

        /* Startup recovery — reset stale sessions */
        {   const char *rsql = "UPDATE sessions SET state='idle' WHERE state IN ('running','llm_running','tool_running','compacting');";
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
        channel_launch_all(g_db);

        /* poll() setup — sized for fixed fds + all children with result pipes */
        int max_pfds = 5 + CHILD_MAX;  /* chld, wake, fifo, worker, result pipes */
        struct pollfd *pfds = malloc(max_pfds * sizeof(struct pollfd));
        if (!pfds) { perror("malloc"); return 1; }

        /* Daemon event loop */
        while (!shutdown_requested()) {
            /* Rebuild pollfd set each iteration to include active result pipes */
            int nfds = 0;
            pfds[nfds].fd = g_chld_pipe[0]; pfds[nfds].events = POLLIN; nfds++;
            pfds[nfds].fd = wake_fd(); pfds[nfds].events = POLLIN; nfds++;
            int fifo_idx = -1;
            if (fifo_fd >= 0) { fifo_idx = nfds; pfds[nfds].fd = fifo_fd; pfds[nfds].events = POLLIN; nfds++; }
            int d_worker_idx = nfds;
            pfds[nfds].fd = daemon_worker_fd; pfds[nfds].events = POLLIN; nfds++;

            int result_pipe_base = nfds;
            nfds = add_result_pipe_fds(pfds, nfds, max_pfds);

            int rc = poll(pfds, (nfds_t)nfds, 1000);
            if (rc < 0) { if (errno == EINTR) continue; break; }

            drain_ready_result_pipes(pfds, result_pipe_base, nfds);

            if (pfds[0].revents & POLLIN)
                event_step_chld();
            if (pfds[1].revents & POLLIN) {
                WakeMsg msg;
                while (read(wake_fd(), &msg, sizeof(msg)) == (ssize_t)sizeof(msg)) {
                    if (child_has_session(msg.session_id)) continue;
                    run_advance(msg.session_id);
                }
            }
            if (fifo_idx >= 0 && (pfds[fifo_idx].revents & POLLIN)) {
                char drain[64];
                while (read(fifo_fd, drain, sizeof(drain)) > 0) {}
                channel_consume_events(g_db);
            }
            if (pfds[d_worker_idx].revents & POLLIN)
                event_step_worker(daemon_worker_fd);
            if (rc == 0 && g_child_count > 0) reap_children();
            channel_tick(g_db);
            child_sweep_deadlines();
        }

        free(pfds);

        /* Shutdown */
        llm_worker_stop();
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
    if (!host_mode) {
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
    if (host_mode) setenv("CCLAW_TRUST_LEVEL", "host", 1);
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
    /* Allocate for fixed fds + all children with result pipes */
    int cli_max_pfds = 4 + CHILD_MAX;
    struct pollfd *cli_pfds = malloc(cli_max_pfds * sizeof(struct pollfd));
    if (!cli_pfds) { perror("malloc"); return 1; }

    /* Start LLM worker */
    int worker_fd = -1;
    int worker_idx = -1;  /* slot index, recomputed each loop iteration */
    if (llm_worker_start(db_path, g_llm_threads) == 0) {
        worker_fd = llm_worker_fd();
        set_nonblock(worker_fd);
    } else {
        fprintf(stderr, "error: failed to start LLM worker\n");
        agent_setup_destroy(&setup); free(base_dir); config_free(g_cfg); db_close(g_db); free(db_path); return 1;
    }

    /* Register stdin for interactive mode */
    int use_stdin = 0;
    int stdin_idx = -1;   /* slot index, recomputed each loop iteration */
    if (!prompt && isatty(STDIN_FILENO)) {
        set_nonblock(STDIN_FILENO);
        use_stdin = 1;
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
        /* Rebuild pollfd set each iteration to include active result pipes.
         * Slot indexes are recomputed because slot order depends on which
         * fixed fds are present. */
        int cli_nfds = 0;
        worker_idx = -1;
        stdin_idx = -1;
        if (worker_fd >= 0) {
            worker_idx = cli_nfds;
            cli_pfds[cli_nfds].fd = worker_fd; cli_pfds[cli_nfds].events = POLLIN; cli_nfds++;
        }
        if (use_stdin) {
            stdin_idx = cli_nfds;
            cli_pfds[cli_nfds].fd = STDIN_FILENO; cli_pfds[cli_nfds].events = POLLIN; cli_nfds++;
        }
        int chld_idx = cli_nfds;
        cli_pfds[cli_nfds].fd = g_chld_pipe[0]; cli_pfds[cli_nfds].events = POLLIN; cli_nfds++;

        int result_pipe_base = cli_nfds;
        cli_nfds = add_result_pipe_fds(cli_pfds, cli_nfds, cli_max_pfds);

        int nready = poll(cli_pfds, (nfds_t)cli_nfds, 500);
        if (nready < 0) { if (errno == EINTR) continue; break; }

        drain_ready_result_pipes(cli_pfds, result_pipe_base, cli_nfds);

        if (cli_pfds[chld_idx].revents & POLLIN) {
            event_step_chld();

            if (g_mode == 0 && !g_cli_turn_active) {
                if (g_cli_done) goto done;
                printf("> "); fflush(stdout);
            }
        }
        if (worker_idx >= 0 && (cli_pfds[worker_idx].revents & POLLIN)) {
            event_step_worker(worker_fd);

            if (g_mode == 0 && !g_cli_turn_active) {
                if (g_cli_done) goto done;
                printf("> "); fflush(stdout);
            }
        }
        if (stdin_idx >= 0 && (cli_pfds[stdin_idx].revents & (POLLERR | POLLNVAL))) {
            /* stdin fd broken/closed — exit instead of spinning on POLLNVAL */
            LOG_ERROR_(g_cfg, "stdin: poll error (revents=0x%x), exiting",
                       cli_pfds[stdin_idx].revents);
            goto done;
        }
        if (stdin_idx >= 0 && (cli_pfds[stdin_idx].revents & POLLIN)) {
            if (g_cli_turn_active) {
                LOG_DEBUG_(g_cfg, "stdin: POLLIN but turn active, skipping");
                continue;
            }

            /* Read line directly (avoid stdio buffering issues with non-blocking fd) */
            static char linebuf[4096];
            static size_t linepos = 0;
            ssize_t n = read(STDIN_FILENO, linebuf + linepos, sizeof(linebuf) - linepos - 1);
            if (n <= 0) { if (n == 0) goto done; continue; } /* EOF or EAGAIN */
            linepos += (size_t)n;
            linebuf[linepos] = '\0';
            char *nl = strchr(linebuf, '\n');
            if (!nl) continue; /* partial line, wait for more */
            *nl = '\0';
            LOG_DEBUG_(g_cfg, "stdin: read line [%s]", linebuf);

            if (!linebuf[0]) { linepos = 0; printf("> "); fflush(stdout); continue; }
            if (strcmp(linebuf, "exit") == 0 || strcmp(linebuf, "quit") == 0) goto done;

            cli_start_turn(linebuf);
            /* Shift any remaining data after the newline */
            size_t consumed = (size_t)(nl - linebuf) + 1;
            linepos -= consumed;
            if (linepos > 0) memmove(linebuf, nl + 1, linepos);
        }

        if (nready == 0 && g_child_count > 0) reap_children();
        child_sweep_deadlines();
    }

done:
    /* Print session cost */
    { int64_t cost = session_cost(g_db, g_cli_session);
      if (cost > 0) fprintf(stderr, "\n[session cost: $%.6f]\n", (double)cost / 1e9); }

    session_set_state(g_db, g_cli_session, "idle");
    agent_setup_destroy(&setup);
    llm_worker_stop();
    close(g_chld_pipe[0]); close(g_chld_pipe[1]);
    free(cli_pfds);
    free(base_dir); config_free(g_cfg); db_close(g_db); free(db_path);
    return rc;
}
