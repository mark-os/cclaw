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
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cclaw.h"
#include "config.h"
#include "log.h"
#include "sandbox.h"
#include "tool_js.h"
#include "qjs_helpers.h"
#include "agent_config.h"
#include "agent_setup.h"
#include "hook_dispatch.h"
#include "approval.h"
#include "llm_proc.h"
#include "llm_worker.h"
#include "tools.h"
#include "tool_parse.h"
#include "tool_request_config.h"
#include "context.h"
#include "db.h"
#include "templates.h"
#include "db_response.h"
#include "shutdown.h"
#include "wake.h"
#include "advance.h"
#include "channel.h"
#include "channel_api.h"
#include "channel_runner.h"
#include "secret.h"
#include "secret_scan.h"
#include "tool_policy.h"
#include "secret_interp.h"
#include "run_tool.h"
#include "tool_file.h"
#include "resolve.h"
#include "web.h"
#include "heartbeat.h"
#include "cron.h"

_Static_assert(sizeof(WakeMsg) <= PIPE_BUF,
    "WakeMsg must fit in PIPE_BUF so wake-pipe writes stay atomic");

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
    char tool_name[64];     /* for the §8 observer hook */
    char *tool_args;        /* strdup'd args for the observer; freed on cleanup */
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
    int timeout_sec;        /* window used for `deadline`, for the timeout message */
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
    free(c->tool_args);
    c->tool_args = NULL;
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
static int spawn_run_tool_child(int64_t session_id, const char *agent_name,
                                const char *tool_call_id, const char *tool_name,
                                const char *tool_args, int64_t turn_id,
                                int64_t entry_id, const char *blob, size_t blob_len,
                                int timeout_sec);

static sqlite3 *g_db;
static Config *g_cfg;
static int g_mode;  /* 0=cli, 1=daemon */
static char g_instance_id[40];  /* this process's registry instance id ("" until registered) */
static int g_llm_threads = 4;     /* worker thread pool size */
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
        Message msg = {.role = ROLE_ASSISTANT,
                       .content = "error: max iterations reached",
                       .stop_reason = STOP_REASON_ERROR};
        entry_append_with_turn(g_db, session_id, &msg, 0);
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
    int rc = llm_worker_submit(g_db, session_id, agent_name, iteration == 0 ? 1 : 0);
    if (rc < 0) {
        /* No worker accepted the job — don't strand the session in llm_running
         * (no completion will ever fire to move it off). Revert to idle. */
        session_set_state(g_db, session_id, "idle");
    }
    return rc;
}

/* Forward declarations for approval + advance (used by fork_tool_exec and resolve_approval) */
static void run_advance(int64_t session_id);
static void handle_approval_park(int64_t session_id);
/* resolve_approval declared in resolve.h (non-static) */

/* ── fork_tool_exec ─────────────────────────────────────────────── */

/* Tools that run in-process (need parent's DB handle or user interaction) */
static int tool_is_inline(const char *name) {
    return strcmp(name, "request_config") == 0 ||
           strcmp(name, "memory_create") == 0 ||
           strcmp(name, "memory_append") == 0 ||
           strcmp(name, "memory_replace") == 0 ||
           strcmp(name, "db_query") == 0 ||
           strcmp(name, "launch_agent") == 0 ||
           strcmp(name, "check_session") == 0 ||
           strcmp(name, "check_approval") == 0 ||
           strcmp(name, "extension_promote") == 0 ||
           strcmp(name, "extension_publish") == 0 ||
           strcmp(name, "extension_attach") == 0 ||
           strcmp(name, "extension_list") == 0;
}

/* Tools whose sibling calls in one assistant turn may run concurrently.
 * Default is serial (safe): models emit ordered calls — shell especially —
 * expecting sequential side effects on the shared workspace. Only independent,
 * self-contained delegations opt in. Concurrency is a property of the tool. */
static int tool_is_parallel_safe(const char *name) {
    return strcmp(name, "launch_agent") == 0;
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

/* Append an inline error tool-result and mark the call done. `detail` is the
 * status detail column (may be NULL). Always returns 1 (handled inline). */
static int tool_inline_error(int64_t session_id, PendingToolCall *tc,
                             const char *msg, const char *detail) {
    char *err = strdup(msg);
    ToolResult tr = {.tool_call_id = tc->call_id, .content = err};
    Message m = {.role = ROLE_TOOL, .tool_result = &tr,
                 .tool_name = tc->name, .is_error = 1};
    entry_append_with_turn(g_db, session_id, &m, tc->turn_id);
    db_tool_call_set_status(g_db, session_id, tc->call_id, "done", detail);
    free(err);
    return 1;
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

    /* §4+§8 dispatch gate (fixed order: capability ceiling → gating hook →
     * approval gate). The gating hook may only *raise* restriction
     * (silent→ask, ask→deny) — a hook can veto, only a grant authorizes.
     * Re-entrant (§7a): the approval is matched by (session_id, tool_call_id),
     * so a denial answers this call and an approval re-runs the same frozen
     * tool_call once (the approval is consumed on use). */
    {
        /* Authorization floor: a tool with no live grant is denied outright.
         * agent_tool_mode's run-freely default (SILENT) describes *how* a
         * granted tool is gated, not *whether* the tool is authorized — so the
         * grant must be checked first or an ungranted tool would slip through
         * as SILENT→ALLOW. */
        if (!grants_contains(g_db, agent_name, "tool", tc->name)) {
            char err[160];
            snprintf(err, sizeof(err),
                     "error: %s not granted — request it with request_config",
                     tc->name);
            ToolResult tr = {.tool_call_id = tc->call_id, .content = err};
            Message msg = {.role = ROLE_TOOL, .tool_result = &tr,
                           .tool_name = tc->name, .is_error = 1};
            entry_append_with_turn(g_db, session_id, &msg, tc->turn_id);
            db_tool_call_set_status(g_db, session_id, tc->call_id, "done", "not_granted");
            return 1;
        }
        ToolApprovalMode mode = agent_tool_mode(g_db, agent_name, tc->name);
        HookGate gate = (mode == TOOL_MODE_SILENT) ? HOOK_GATE_ALLOW : HOOK_GATE_ASK;

        /* Per-argument policy pre-filter (restrict-only, before hooks) */
        if (te->policy_json) {
            PolicyEffect pe = policy_eval(tc->arguments, te->policy_json);
            if (pe == POLICY_DENY) {
                char err[128];
                snprintf(err, sizeof(err), "error: %s denied by policy", tc->name);
                ToolResult tr = {.tool_call_id = tc->call_id, .content = err};
                Message msg = {.role = ROLE_TOOL, .tool_result = &tr,
                               .tool_name = tc->name, .is_error = 1};
                entry_append_with_turn(g_db, session_id, &msg, tc->turn_id);
                db_tool_call_set_status(g_db, session_id, tc->call_id, "done", "policy:deny");
                return 1;
            }
            if (pe == POLICY_ASK && gate < HOOK_GATE_ASK)
                gate = HOOK_GATE_ASK;
        }

        if (g_tool_setup) {
            char *reason = NULL;
            HookGate h = hook_dispatch_gate_tool_call(&g_tool_setup->ext_ctx, g_db,
                                                      tc->name, tc->arguments, &reason);
            if (h > gate) gate = h;  /* restrict-only: most restrictive wins */
            if (gate == HOOK_GATE_DENY) {
                char err[256];
                snprintf(err, sizeof(err), "error: %s blocked by hook%s%s", tc->name,
                         reason ? ": " : "", reason ? reason : "");
                ToolResult tr = {.tool_call_id = tc->call_id, .content = err};
                Message msg = {.role = ROLE_TOOL, .tool_result = &tr,
                               .tool_name = tc->name, .is_error = 1};
                entry_append_with_turn(g_db, session_id, &msg, tc->turn_id);
                db_tool_call_set_status(g_db, session_id, tc->call_id, "done", "hook:deny");
                free(reason);
                return 1;
            }
            free(reason);
        }
        if (gate == HOOK_GATE_ASK) {
            Approval *ap = approval_get_for_tool_call(g_db, session_id, tc->call_id);
            int approved = ap && ap->state && strcmp(ap->state, "approved") == 0;
            int denied   = ap && ap->state && strcmp(ap->state, "denied") == 0;
            int pending  = ap && ap->state && strcmp(ap->state, "pending") == 0;
            if (denied) {
                char err[128];
                snprintf(err, sizeof(err), "error: %s denied by user", tc->name);
                ToolResult tr = {.tool_call_id = tc->call_id, .content = err};
                Message msg = {.role = ROLE_TOOL, .tool_result = &tr,
                               .tool_name = tc->name, .is_error = 1};
                entry_append_with_turn(g_db, session_id, &msg, tc->turn_id);
                db_tool_call_set_status(g_db, session_id, tc->call_id, "done", "denied");
                approval_free(ap);
                return 1;
            }
            if (!approved) {
                /* none → create + park; pending → re-park idempotently. */
                if (!pending)
                    approval_create(g_db, session_id, tc->call_id, tc->name,
                                    tc->name, tc->arguments, "rerun");
                session_set_state(g_db, session_id, "awaiting_approval");
                approval_free(ap);
                handle_approval_park(session_id);
                return 2; /* parked */
            }
            /* approved → consume (single-use) and fall through to execute the
             * frozen call. An ALWAYS decision flips the tool mode to silent so
             * it never reaches the gate again; only a "once" approval lands
             * here, and consuming it stops a replayed tool_call_id from
             * re-using the same grant. */
            approval_consume(g_db, ap->id);
            approval_free(ap);
        }
    }

    /* Inline tools: execute in parent process */
    if (tool_is_inline(tc->name)) {
        /* Interpolate {{SECRET:X}} only for tools that exec with credentials */
        char *interp_args = NULL;
        if (tool_needs_interpolation(tc->name) && g_tool_setup && g_tool_setup->secret_count > 0)
            interp_args = secret_interpolate(tc->arguments, g_tool_setup->secrets, g_tool_setup->secret_count);
        /* Thread the live session + tool_call_id into the per-tool context.
         * g_tool_setup is a single shared instance, so the session_id captured
         * at agent_setup_init time is stale (0 in CLI) — the dispatching session
         * varies per call (root or any sub-agent). launch_agent uses it as the
         * child's parent, so without this the child gets parent -1 and its
         * result can never route back. check_session shares the same ctx. */
        if ((strcmp(tc->name, "launch_agent") == 0 ||
             strcmp(tc->name, "check_session") == 0) && te->user_data) {
            AgentLaunchCtx *lc = (AgentLaunchCtx *)te->user_data;
            lc->session_id = session_id;
            lc->current_tool_call_id = tc->call_id;
        }
        if (strcmp(tc->name, "request_config") == 0 && te->user_data)
            ((RequestConfigCtx *)te->user_data)->current_tool_call_id = tc->call_id;
        char *result = te->handler(interp_args ? interp_args : tc->arguments, te->user_data);
        if (interp_args) { explicit_bzero(interp_args, strlen(interp_args)); free(interp_args); }
        /* A NULL result means the tool dispatched async work and left this
         * tool_call without an inline result. Two distinct shapes: */
        if (!result && strcmp(tc->name, "launch_agent") == 0) {
            /* Sub-agent launched. Mark this call 'running' so the turn-join
             * (advance_session, tool_running) neither re-dispatches it nor
             * proceeds to the LLM until the child completes and writes the
             * result keyed by this call_id. The parent stays tool_running, so
             * sibling launch_agent calls keep dispatching → real parallelism. */
            db_tool_call_set_status(g_db, session_id, tc->call_id, "running", NULL);
            /* parallel-safe tools let dispatch continue to siblings (3);
             * a serial tool would stop and wait (0). */
            return tool_is_parallel_safe(tc->name) ? 3 : 0;
        }
        if (!result && strcmp(tc->name, "request_config") == 0) {
            /* Approval gate: the session is parked in awaiting_approval; the
             * tool_call stays pending until resolve_approval writes the result. */
            handle_approval_park(session_id);
            return 2; /* parked, don't advance */
        }
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
        /* §8 observer hook (after execution; side-effect only) */
        if (g_tool_setup)
            hook_dispatch_observe_tool_call(&g_tool_setup->ext_ctx, g_db,
                                            tc->name, tc->arguments, result);
        free(stored);
        free(result);
        return 1; /* Handled inline */
    }

    /* ── File-tier re-exec path: fork+exec a clean --run-tool child ────
     * Replaces the fork-only path for file tools when sandbox is required.
     * The daemon is multithreaded; fork-without-exec is UB (frozen locks in
     * child). The re-exec'd child is single-threaded and sets up its own
     * namespace sandbox. */
    if (te->user_data && (strcmp(tc->name, "file_read") == 0 ||
                          strcmp(tc->name, "file_write") == 0 ||
                          strcmp(tc->name, "file_edit") == 0 ||
                          strcmp(tc->name, "file_list") == 0 ||
                          strcmp(tc->name, "file_find") == 0 ||
                          strcmp(tc->name, "file_grep") == 0)) {
        FileReadCtx *fctx = (FileReadCtx *)te->user_data;
        if (fctx->sandbox && fctx->workspace) {
            /* CLI progress */
            if (g_mode == 0)
                cli_print_tool_call(tc->name, tc->arguments);
            session_set_state(g_db, session_id, "tool_running");

            size_t blob_len = 0;
            char *blob = run_tool_serialize_file_request(
                tc->name, tc->arguments, fctx->workspace,
                (const char **)fctx->read_paths, fctx->read_path_count,
                (const char **)fctx->write_paths, fctx->write_path_count,
                fctx->workspace_ro, fctx->mount_cwd, fctx->cwd_path,
                fctx->env_mode,
                fctx->rlimits.nproc, fctx->rlimits.as_mb, fctx->rlimits.cpu_sec,
                &blob_len);
            if (!blob)
                return tool_inline_error(session_id, tc,
                    "error: file tool request exceeds 32KB cap", NULL);
            int rc = spawn_run_tool_child(session_id, agent_name,
                         tc->call_id, tc->name, tc->arguments,
                         tc->turn_id, tc->entry_id, blob, blob_len, 120);
            free(blob);
            if (rc != 0)
                return tool_inline_error(session_id, tc,
                    "error: spawn_run_tool_child failed", "fork_failed");
            db_tool_call_set_status(g_db, session_id, tc->call_id, "running", NULL);
            return 0; /* async serial — wait for reap */
        }
        /* else: sandbox==0 (host mode) — fall through to generic fork path
         * which runs the handler in-process via file_sandbox_run's shortcut */
    }

    /* ── Shell-tier re-exec path: fork+exec a clean --run-tool broker ────
     * A broker is interposed IFF the tier needs gated egress (web, shell).
     * The broker IS the --run-tool process — fork+exec, never fork-only.
     * Network-less tiers (file) spawn directly via spawn_run_tool_child.
     * Secrets: interpolated HERE in the daemon parent, blob carries resolved
     * values. Broker/child never hold the master key. */
    if (strcmp(tc->name, "shell_exec") == 0 && te->user_data) {
        ShellConfig *sc = (ShellConfig *)te->user_data;
        if (sc->sandbox && sc->workspace) {
            if (g_mode == 0)
                cli_print_tool_call(tc->name, tc->arguments);
            session_set_state(g_db, session_id, "tool_running");

            /* Parse arguments to extract command + timeout */
            ToolArgs ta;
            if (tool_parse(tc->arguments, &ta) != 0)
                return tool_inline_error(session_id, tc,
                    "error: invalid shell_exec arguments", NULL);
            const char *command = targ_str(&ta, "command");
            int cmd_timeout = targ_int(&ta, "timeout", sc->timeout);
            if (cmd_timeout <= 0) cmd_timeout = sc->timeout;

            /* Parent-side secret interpolation (daemon holds the key) */
            char *interp_cmd = NULL;
            if (g_tool_setup && g_tool_setup->secret_count > 0)
                interp_cmd = secret_interpolate(command, g_tool_setup->secrets,
                                                g_tool_setup->secret_count);
            const char *resolved_cmd = interp_cmd ? interp_cmd : command;

            /* Filter secrets to minimal set (only those referenced by command) */
            RunToolSecret *min_secrets = NULL;
            size_t min_count = 0;
            if (sc->secrets && sc->secret_count > 0) {
                min_secrets = malloc(sc->secret_count * sizeof(RunToolSecret));
                if (min_secrets) {
                    for (size_t i = 0; i < sc->secret_count; i++) {
                        /* Check if command references $CCLAW_SECRET_<name> */
                        char tok[256];
                        int tlen = snprintf(tok, sizeof(tok), "CCLAW_SECRET_%s", sc->secrets[i].name);
                        if (tlen <= 0 || tlen >= (int)sizeof(tok)) continue;
                        int found = 0;
                        for (const char *p = resolved_cmd; (p = strstr(p, tok)) != NULL; p += tlen) {
                            /* Require a word boundary on BOTH sides: a bare
                             * substring of a larger identifier (e.g. the env
                             * name MY_CCLAW_SECRET_FOO) is not a reference to
                             * CCLAW_SECRET_FOO and must not pull that secret in. */
                            char before = (p == resolved_cmd) ? '\0' : p[-1];
                            char after = p[tlen];
                            int before_ident = (before >= 'A' && before <= 'Z') ||
                                                (before >= 'a' && before <= 'z') ||
                                                (before >= '0' && before <= '9') || before == '_';
                            int after_ident = (after >= 'A' && after <= 'Z') ||
                                               (after >= 'a' && after <= 'z') ||
                                               (after >= '0' && after <= '9') || after == '_';
                            if (!before_ident && !after_ident) { found = 1; break; }
                        }
                        if (found) {
                            min_secrets[min_count].name = sc->secrets[i].name;
                            min_secrets[min_count].value = sc->secrets[i].value;
                            min_count++;
                        }
                    }
                }
            }

            /* Resolve agent_dir for proxy socket */
            char agent_dir[PATH_MAX];
            agent_dir_resolve(sc->workspace, sc->db_path, agent_dir, sizeof(agent_dir));

            size_t blob_len = 0;
            char *blob = run_tool_serialize_shell_request(
                resolved_cmd, cmd_timeout,
                sc->workspace, sc->cwd_path, agent_dir,
                (const char **)sc->allowed_hosts, sc->allowed_host_count,
                min_secrets, min_count,
                sc->sandbox, sc->env_mode, sc->net_mode, sc->mount_cwd,
                sc->workspace_ro,
                sc->rlimits.nproc, sc->rlimits.as_mb, sc->rlimits.cpu_sec,
                (const char **)sc->read_paths, sc->read_path_count,
                (const char **)sc->write_paths, sc->write_path_count,
                &blob_len);

            /* Wipe interpolated command (contains secret plaintext) */
            if (interp_cmd) { explicit_bzero(interp_cmd, strlen(interp_cmd)); free(interp_cmd); }
            free(min_secrets);
            tool_parse_free(&ta);

            if (!blob)
                return tool_inline_error(session_id, tc,
                    "error: shell request exceeds 32KB cap", NULL);

            /* Daemon backstop fires margin-seconds AFTER the broker's own
             * cmd_timeout, so the broker's (well-tested) teardown — which kills
             * the sandbox child's process group — wins in the normal case. */
            int rc = spawn_run_tool_child(session_id, agent_name,
                         tc->call_id, tc->name, tc->arguments,
                         tc->turn_id, tc->entry_id, blob, blob_len,
                         cmd_timeout + 30);
            /* Wipe blob (carries interpolated secrets) */
            explicit_bzero(blob, blob_len);
            free(blob);
            if (rc != 0)
                return tool_inline_error(session_id, tc,
                    "error: spawn_run_tool_child failed", "fork_failed");
            db_tool_call_set_status(g_db, session_id, tc->call_id, "running", NULL);
            return 0;
        }
        /* else: sandbox==0 (host mode) — fall through to generic fork path */
    }

    /* Forkable tool: pipe for result capture. A pipe()/fork() failure must not
     * leave the call pending forever — the session has already moved to
     * tool_running and no wake resurrects an un-dispatched call. Synthesize an
     * error result and complete the call inline, mirroring the unknown-tool
     * branch, so the turn can advance. */
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        char err[160];
        snprintf(err, sizeof(err), "error: %s: pipe failed: %s",
                 tc->name, strerror(errno));
        ToolResult tr = {.tool_call_id = tc->call_id, .content = err};
        Message msg = {.role = ROLE_TOOL, .tool_result = &tr,
                       .tool_name = tc->name, .is_error = 1};
        entry_append_with_turn(g_db, session_id, &msg, tc->turn_id);
        db_tool_call_set_status(g_db, session_id, tc->call_id, "done", "fork_failed");
        return 1;
    }

    /* CLI progress */
    if (g_mode == 0)
        cli_print_tool_call(tc->name, tc->arguments);

    session_set_state(g_db, session_id, "tool_running");

    pid_t pid = fork();
    if (pid < 0) {
        char err[160];
        snprintf(err, sizeof(err), "error: %s: fork failed: %s",
                 tc->name, strerror(errno));
        close(pipefd[0]); close(pipefd[1]);
        ToolResult tr = {.tool_call_id = tc->call_id, .content = err};
        Message msg = {.role = ROLE_TOOL, .tool_result = &tr,
                       .tool_name = tc->name, .is_error = 1};
        entry_append_with_turn(g_db, session_id, &msg, tc->turn_id);
        db_tool_call_set_status(g_db, session_id, tc->call_id, "done", "fork_failed");
        return 1;
    }
    if (pid == 0) {
        /* Child: only the output pipe write end is inherited (no O_CLOEXEC).
         * Parent infrastructure fds (DB, sigchld pipe, notify pipe) are
         * O_CLOEXEC and will close on exec within the shell sandbox.
         *
         * No per-subsystem teardown is needed here, and no separate "thin"
         * child binary: COW fork shares the parent's pages read-only, and
         * demand paging keeps any subsystem this child never calls (civetweb,
         * QuickJS, most of SQLite) out of its resident set. The forked C-tool
         * broker only runs the tool handler and writes a pipe — it touches none
         * of that machinery, so those pages stay non-resident in the child. The
         * inherited SQLite handle is left open on purpose: SQLite is not
         * fork-safe, so closing it would risk the parent's shared connection. */
        close(pipefd[0]);
        /* This broker holds no DB handle and never needs the master key — wipe
         * the inherited copy so the relay process carries no key material. */
        db_wipe_secret_key();
        /* Interpolate {{SECRET:X}} only for tools that exec with credentials */
        char *interp_args = NULL;
        if (tool_needs_interpolation(tc->name) && g_tool_setup && g_tool_setup->secret_count > 0)
            interp_args = secret_interpolate(tc->arguments, g_tool_setup->secrets, g_tool_setup->secret_count);
        char *result = te->handler(interp_args ? interp_args : tc->arguments, te->user_data);
        /* Wipe the interpolated args (may carry {{SECRET}} plaintext) before the
         * broker writes its result to the pipe and exits. */
        if (interp_args) { explicit_bzero(interp_args, strlen(interp_args)); free(interp_args); }
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
    c->timeout_sec = 120;
    c->deadline = time(NULL) + c->timeout_sec;
    snprintf(c->agent_name, sizeof(c->agent_name), "%s", agent_name);
    snprintf(c->tool_call_id, sizeof(c->tool_call_id), "%s", tc->call_id);
    snprintf(c->tool_name, sizeof(c->tool_name), "%s", tc->name);
    c->tool_args = tc->arguments ? strdup(tc->arguments) : NULL;  /* for §8 observer */
    /* In flight: exclude from get_pending so a re-advance (another wake landing
     * while this child runs) can't fork a duplicate. reap_children flips it to
     * 'done' via db_tool_call_complete_with_result. */
    db_tool_call_set_status(g_db, session_id, tc->call_id, "running", NULL);
    /* Forked tools are serial by default (0): the next pending call waits for
     * this child to be reaped. A tool marked parallel-safe continues (3) so
     * siblings fork concurrently — the poll loop already multiplexes N pipes. */
    return tool_is_parallel_safe(tc->name) ? 3 : 0;
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

/* ── spawn_run_tool_child: fork+exec a --run-tool child ──────────── */

#define FD_REQUEST RUNTOOL_FD_REQUEST  /* the socketpair fd in the child */

/* Spawn a sandboxed tool child via fork+execve. The request blob is sent
 * over a socketpair (fd 3 in the child). Returns 0 on success (child is
 * registered in g_children), -1 on failure (error result written inline). */
static int spawn_run_tool_child(int64_t session_id, const char *agent_name,
                                const char *tool_call_id, const char *tool_name,
                                const char *tool_args, int64_t turn_id,
                                int64_t entry_id, const char *blob, size_t blob_len,
                                int timeout_sec) {
    if (g_child_count >= CHILD_MAX) return -1;

    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) return -1;

    /* sp[0] = parent side, sp[1] = child side (becomes fd 3) */
    /* Set O_CLOEXEC on parent side so it doesn't leak into other children */
    fcntl(sp[0], F_SETFD, FD_CLOEXEC);
    /* Child side must NOT have CLOEXEC (it becomes fd 3 post-dup2) */

    pid_t pid = fork();
    if (pid < 0) {
        close(sp[0]); close(sp[1]);
        return -1;
    }
    if (pid == 0) {
        /* CHILD: async-signal-safe only. No malloc, no stdio, no snprintf. */
        close(sp[0]);
        /* dup2 child socket to fd 3 */
        if (sp[1] != FD_REQUEST) {
            dup2(sp[1], FD_REQUEST);
            close(sp[1]);
        }
        /* execve self as --run-tool. Minimal env (inherits nothing sensitive). */
        char *const argv[] = {"cclaw", "--run-tool", NULL};
        char *const envp[] = {NULL};
        execve("/proc/self/exe", argv, envp);
        /* execve failed — write static error to fd 3 and die */
        const char *err = "error: execve failed";
        (void)write(FD_REQUEST, err, 20);
        _exit(127);
    }

    /* PARENT: close child end, write request blob (blocking, safe because
     * blob is capped at RUNTOOL_REQUEST_MAX < kernel socket buffer) */
    close(sp[1]);
    ssize_t written = 0;
    size_t total = blob_len;
    while ((size_t)written < total) {
        ssize_t w = write(sp[0], blob + written, total - (size_t)written);
        if (w <= 0) {
            close(sp[0]);
            waitpid(pid, NULL, 0);
            return -1;
        }
        written += w;
    }

    /* Switch to nonblocking for result reads in poll loop */
    set_nonblock(sp[0]);

    /* Register child — mirrors fork_tool_exec bookkeeping */
    ChildProc *c = &g_children[g_child_count++];
    memset(c, 0, sizeof(*c));
    c->pid = pid;
    c->type = CHILD_TOOL_EXEC;
    c->session_id = session_id;
    c->turn_id = turn_id;
    c->entry_id = entry_id;
    c->result_pipe = sp[0];
    c->outbuf = NULL;
    c->outbuf_len = 0;
    c->timeout_sec = timeout_sec > 0 ? timeout_sec : 120;
    c->deadline = time(NULL) + c->timeout_sec;
    snprintf(c->agent_name, sizeof(c->agent_name), "%s", agent_name);
    snprintf(c->tool_call_id, sizeof(c->tool_call_id), "%s", tool_call_id);
    snprintf(c->tool_name, sizeof(c->tool_name), "%s", tool_name);
    c->tool_args = tool_args ? strdup(tool_args) : NULL;
    return 0;
}

/* ── reap_children (state machine) ──────────────────────────────── */

/* ── compute_timeout_ms: dynamic poll() timeout ─────────────── */

#define POLL_DB_INTERVAL 5   /* seconds between DB polls (approvals, future cron) */
#define POLL_MAX_SLEEP   30  /* upper bound on poll() sleep */

static time_t g_next_db_poll;  /* next time DB periodic work is due */

static int compute_timeout_ms(void) {
    time_t now = time(NULL);
    time_t nearest = now + POLL_MAX_SLEEP;

    /* Tier 1: in-memory deadlines (precise) */
    for (int i = 0; i < g_child_count; i++) {
        time_t d = g_children[i].deadline;
        if (d > 0 && d < nearest)
            nearest = d;
    }
    if (g_mode == 1) {
        time_t cd = channel_next_deadline();
        if (cd > 0 && cd < nearest)
            nearest = cd;
    }

    /* Tier 2: periodic DB poll */
    if (g_next_db_poll < nearest)
        nearest = g_next_db_poll;

    int ms = (int)((nearest - now) * 1000);
    return ms < 0 ? 0 : ms;
}

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
            char errbuf[64];
            snprintf(errbuf, sizeof(errbuf), "error: tool timed out (%ds)",
                     c->timeout_sec > 0 ? c->timeout_sec : 120);
            ToolResult tr = {.tool_call_id = c->tool_call_id, .content = errbuf};
            Message msg = {.role = ROLE_TOOL, .tool_result = &tr,
                           .tool_name = "", .is_error = 1};
            entry_append_with_turn(g_db, c->session_id, &msg, c->turn_id);
            db_tool_call_complete_with_result(g_db, c->entry_id, c->tool_call_id, -1);
        }
        c->deadline = -1; /* mark consumed so reap doesn't double-advance */
    }
}

/* ── approval_sweep_expired: deny timed-out pending approvals ── */

static void approval_sweep_expired(void) {
    int n = 0;
    int64_t *ids = approval_list_expired(g_db, g_instance_id, &n);
    for (int i = 0; i < n; i++)
        resolve_approval(ids[i], APPROVAL_DENY, "auto:expired");
    free(ids);
}

/* approval_block_sec (KV, default 60), clamped to approval_timeout_sec so the
 * short block never outlasts the final expiry deadline. */
static int approval_block_seconds(void) {
    int block = 60;
    char *kv = db_kv_get(g_db, "approval_block_sec");
    if (kv) { long v = strtol(kv, NULL, 10); if (v > 0) block = (int)v; free(kv); }
    int timeout = 3600;
    char *tv = db_kv_get(g_db, "approval_timeout_sec");
    if (tv) { long v = strtol(tv, NULL, 10); if (v > 0) timeout = (int)v; free(tv); }
    if (block > timeout) block = timeout;
    return block;
}

/* Unpark one approval past the short block window: answer the frozen tool_call
 * with a non-terminal "still pending" result and resume the turn, leaving the
 * approval pending so a later decision is delivered async (post-window). Under
 * BEGIN IMMEDIATE because db_tool_call_set_status is not itself a CAS — re-check
 * the parked invariant before mutating so a concurrent resolve can't double-act. */
static void approval_unpark_block_window(int64_t approval_id) {
    if (sqlite3_exec(g_db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK)
        return;

    int64_t session_id = -1;
    char call_id[128] = {0}, tool_name[128] = {0};
    int ok = 0;
    sqlite3_stmt *s;
    const char *sel =
        "SELECT a.session_id, a.tool_call_id, a.tool_name FROM approvals a"
        " JOIN sessions s ON s.id = a.session_id"
        " JOIN tool_calls t ON t.session_id = a.session_id AND t.call_id = a.tool_call_id"
        " WHERE a.id=? AND a.state='pending' AND s.state='awaiting_approval'"
        "   AND t.status='pending';";
    if (sqlite3_prepare_v2(g_db, sel, -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, approval_id);
        if (sqlite3_step(s) == SQLITE_ROW) {
            session_id = sqlite3_column_int64(s, 0);
            const char *cid = (const char *)sqlite3_column_text(s, 1);
            const char *tn = (const char *)sqlite3_column_text(s, 2);
            if (cid) snprintf(call_id, sizeof(call_id), "%s", cid);
            if (tn) snprintf(tool_name, sizeof(tool_name), "%s", tn);
            ok = cid != NULL;
        }
        sqlite3_finalize(s);
    }
    if (!ok) { sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL); return; }

    char buf[256];
    snprintf(buf, sizeof(buf),
             "approval #%lld still pending — you'll be notified when it's "
             "decided; continuing for now.", (long long)approval_id);
    ToolResult tr = { .tool_call_id = call_id, .content = buf };
    Message msg = { .role = ROLE_TOOL, .tool_result = &tr,
                    .tool_name = tool_name, .is_error = 0 };
    if (entry_append_with_turn(g_db, session_id, &msg, 0) < 0 ||
        db_tool_call_set_status(g_db, session_id, call_id, "done", "block_window") != 0 ||
        session_set_state(g_db, session_id, "tool_running") != 0) {
        sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
        return;
    }
    if (sqlite3_exec(g_db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK)
        return;

    wake_session(session_id);
    run_advance(session_id);
}

/* ── approval_sweep_block_window: unpark approvals past the short block ── */

static void approval_sweep_block_window(void) {
    int block = approval_block_seconds();
    int n = 0;
    int64_t *ids = approval_list_block_due(g_db, block, g_instance_id, &n);
    for (int i = 0; i < n; i++)
        approval_unpark_block_window(ids[i]);
    free(ids);
}

/* ── session_sweep_inbox: backstop for idle sessions with queued work ──
 * Edge wakes (wake pipe / worker / reap) are process-local, so a peer can leave
 * an inbox row on a now-idle session without any live process holding an edge
 * for it (e.g. a cross-process post-window approval delivered by a -p run that
 * then exits). This catches that orphan ≤ POLL_DB_INTERVAL late — a backstop,
 * not the scheduler. advance_session claims an idle session atomically (BEGIN
 * IMMEDIATE + inbox-consume + owner-stamping CAS), so a concurrent edge can't
 * double-dispatch: the loser consumes nothing and NOOPs. Daemon only — a
 * transient CLI is scoped to its own session and must not adopt orphans. */
static void session_sweep_inbox(void) {
    const char *sql =
        "SELECT s.id FROM sessions s WHERE s.state='idle'"
        "  AND EXISTS (SELECT 1 FROM inbox i"
        "              WHERE i.session_id=s.id AND i.consumed=0)"
        " LIMIT 64;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return;
    int cap = 0, n = 0;
    int64_t *ids = NULL;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n >= cap) { cap = cap ? cap * 2 : 16; ids = realloc(ids, (size_t)cap * sizeof(*ids)); }
        ids[n++] = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    for (int i = 0; i < n; i++)
        if (!child_has_session(ids[i])) run_advance(ids[i]);
    free(ids);
}

/* ── db_periodic: recurring DB housekeeping. Owner-scoped recovery is safe to
 * repeat here (it reclaims only dead-owned sessions), so CLI and daemon peers
 * can both keep the shared DB consistent. ── */

static void db_periodic(void) {
    process_heartbeat(g_db, g_instance_id);
    process_gc_dead(g_db, PROCESS_TTL_SEC);
    db_recover_stale_sessions(g_db);   /* owner-scoped, safe to repeat */
    approval_sweep_block_window();
    approval_sweep_expired();
    if (g_mode == 1) {
        session_sweep_inbox();
        cron_run_due(g_db);
    }
}

/* ── apply_grant: apply an 'apply'-style capability grant ──────────
 * Extracted from resolve_approval so both the in-window path and the
 * post-window inbox path apply grants identically. Called only for an approved
 * (APPROVAL_ALWAYS) apply approval. Sets *rename_failed if a rename's disk step
 * failed (DB change rolled back); refreshes live caps on success. */
static void apply_grant(const Approval *a, const char *agent, int *rename_failed) {
    *rename_failed = 0;
    const char *refresh_agent = agent;
    if (strcmp(a->action, "grant_tool") == 0) {
        ToolArgs ta; tool_parse(a->args_json, &ta);
        const char *v = targ_str(&ta, "tool");
        if (v) agent_config_grant(g_db, agent, "tool", v, 0);
        tool_parse_free(&ta);
    } else if (strcmp(a->action, "grant_host") == 0) {
        ToolArgs ta; tool_parse(a->args_json, &ta);
        const char *v = targ_str(&ta, "host");
        if (v) agent_config_grant(g_db, agent, "host", v, 0);
        tool_parse_free(&ta);
    } else if (strcmp(a->action, "grant_path") == 0) {
        ToolArgs ta; tool_parse(a->args_json, &ta);
        const char *v = targ_str(&ta, "path");
        if (v) agent_config_grant(g_db, agent, "write_path", v, 0);
        tool_parse_free(&ta);
    } else if (strcmp(a->action, "rename_agent") == 0) {
        ToolArgs ta; tool_parse(a->args_json, &ta);
        const char *nn = targ_str(&ta, "name");
        const char *pr = targ_str(&ta, "preamble");
        if (nn) {
            int rc = agent_rename(g_db, agent, nn, a->session_id);
            if (rc == 0 && g_tool_setup) {
                /* Disk rename */
                RequestConfigCtx *rctx = &g_tool_setup->req_cfg_ctx;
                if (rctx->agents_dir) {
                    char old_path[512], new_path[512];
                    snprintf(old_path, sizeof(old_path), "%s/%s", rctx->agents_dir, agent);
                    snprintf(new_path, sizeof(new_path), "%s/%s", rctx->agents_dir, nn);
                    struct stat st;
                    if (stat(old_path, &st) == 0) {
                        if (rename(old_path, new_path) != 0) {
                            /* Rollback DB rename on disk failure */
                            agent_rename(g_db, nn, agent, a->session_id);
                            *rename_failed = 1;
                            goto rename_done;
                        }
                    }
                }
                /* Optional preamble update */
                if (pr && pr[0]) {
                    const char *sql = "UPDATE agents SET system_prompt=? WHERE name=?";
                    sqlite3_stmt *s;
                    if (sqlite3_prepare_v2(g_db, sql, -1, &s, NULL) == SQLITE_OK) {
                        sqlite3_bind_text(s, 1, pr, -1, SQLITE_STATIC);
                        sqlite3_bind_text(s, 2, nn, -1, SQLITE_STATIC);
                        sqlite3_step(s); sqlite3_finalize(s);
                    }
                }
                /* Update live agent name — use nn for subsequent refresh */
                snprintf((char *)rctx->agent_name, 64, "%s", nn);
                setenv("CCLAW_AGENT_NAME", nn, 1);
                refresh_agent = rctx->agent_name;
            }
        }
rename_done:
        tool_parse_free(&ta);
    }
    /* Only rebind the shared setup when the grant target is the bound agent
     * (CLI root). A grant applied to a sub-agent must not leave root's setup
     * carrying its caps; the dispatch path re-binds per call. */
    if (g_tool_setup && !*rename_failed && strcmp(refresh_agent, g_agent_name) == 0)
        agent_setup_refresh_caps(g_tool_setup, g_db, refresh_agent);
}

/* Post-window iff the block sweep already advanced this turn: the frozen
 * tool_call is no longer pending OR the session is no longer awaiting_approval.
 * A genuinely missing tool_call also reads as post-window (deliver via inbox). */
static int approval_is_post_window(int64_t session_id, const char *tool_call_id) {
    char tcs[32] = {0}, ss[32] = {0};
    sqlite3_stmt *s;
    const char *sql =
        "SELECT (SELECT status FROM tool_calls WHERE session_id=?1 AND call_id=?2"
        "        ORDER BY id DESC LIMIT 1),"
        "       (SELECT state FROM sessions WHERE id=?1);";
    if (sqlite3_prepare_v2(g_db, sql, -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, session_id);
        if (tool_call_id) sqlite3_bind_text(s, 2, tool_call_id, -1, SQLITE_STATIC);
        else sqlite3_bind_null(s, 2);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const char *t = (const char *)sqlite3_column_text(s, 0);
            const char *st = (const char *)sqlite3_column_text(s, 1);
            if (t) snprintf(tcs, sizeof tcs, "%s", t);
            if (st) snprintf(ss, sizeof ss, "%s", st);
        }
        sqlite3_finalize(s);
    }
    return strcmp(tcs, "pending") != 0 || strcmp(ss, "awaiting_approval") != 0;
}

/* Deliver a late decision (block window already lapsed) as a new inbox turn:
 * apply grants here too, but never mutate the (already-answered) turn state or
 * re-run a frozen call out of context. */
static void resolve_approval_post_window(const Approval *a, const char *agent,
                                         ApprovalDecision decision, const char *decided_via) {
    int approved = (decision != APPROVAL_DENY);
    int is_apply = a->resolve && strcmp(a->resolve, "apply") == 0;
    int expired = decided_via && strncmp(decided_via, "auto:", 5) == 0;
    if (is_apply) {
        if (approved && decision == APPROVAL_ALWAYS) {
            int rename_failed = 0;
            apply_grant(a, agent, &rename_failed);
            approval_deliver_postwindow(g_db, a,
                rename_failed ? APPROVAL_PW_APPLY_DENIED : APPROVAL_PW_APPLY_GRANTED);
        } else {
            approval_deliver_postwindow(g_db, a,
                expired ? APPROVAL_PW_EXPIRED : APPROVAL_PW_APPLY_DENIED);
        }
    } else {
        if (approved)
            approval_deliver_postwindow(g_db, a, APPROVAL_PW_RERUN_APPROVED);
        else
            approval_deliver_postwindow(g_db, a,
                expired ? APPROVAL_PW_EXPIRED : APPROVAL_PW_RERUN_DENIED);
    }
}

/* ── resolve_approval: approve/deny a parked approval ────────── */

void resolve_approval(int64_t approval_id, ApprovalDecision decision, const char *decided_via) {
    int approved = (decision != APPROVAL_DENY);
    Approval *a = approval_resolve(g_db, approval_id, approved, decided_via);
    if (!a) return;

    int64_t session_id = a->session_id;
    const char *agent = session_get_agent_name(g_db, session_id);

    /* Block window already lapsed → deliver async, leave the turn untouched. */
    if (approval_is_post_window(session_id, a->tool_call_id)) {
        resolve_approval_post_window(a, agent, decision, decided_via);
        free((char *)agent);
        approval_free(a);
        wake_session(session_id);
        return;
    }

    /* Dispatch on the approval's resolve strategy. */
    if (!a->resolve || strcmp(a->resolve, "rerun") == 0) {
        /* ── "rerun": the frozen tool_call proceeds on approval. ── */
        if (decision == APPROVAL_DENY) {
            if (a->tool_call_id) {
                char buf[160];
                snprintf(buf, sizeof(buf), "error: %s denied (%s)", a->action, decided_via);
                ToolResult tr = { .tool_call_id = a->tool_call_id, .content = buf };
                Message msg = { .role = ROLE_TOOL, .tool_result = &tr,
                                .tool_name = a->action, .is_error = 1 };
                entry_append_with_turn(g_db, session_id, &msg, 0);
                db_tool_call_set_status(g_db, session_id, a->tool_call_id, "done", decided_via);
            }
        } else if (decision == APPROVAL_ALWAYS && agent) {
            /* "Allow and stop asking" — flip the standing mode to silent. */
            agent_config_set_tool_mode(g_db, agent, a->action, "silent");
        }
        session_set_state(g_db, session_id, "tool_running");
        free((char *)agent);
        approval_free(a);
        wake_session(session_id);
        run_advance(session_id);
        return;
    }

    /* ── "apply": the tool's side effect is applied here, not re-run. ── */

    /* "once" is incoherent for apply-style approvals (ambient capabilities
     * have no single-use semantics). Reject as error. */
    if (decision == APPROVAL_ONCE) {
        char err[256];
        snprintf(err, sizeof(err),
                 "error: once-approval invalid for %s", a->action);
        if (a->tool_call_id) {
            ToolResult tr = { .tool_call_id = a->tool_call_id, .content = err };
            Message msg = { .role = ROLE_TOOL, .tool_result = &tr,
                            .tool_name = a->tool_name, .is_error = 1 };
            entry_append_with_turn(g_db, session_id, &msg, 0);
            db_tool_call_set_status(g_db, session_id, a->tool_call_id, "done", decided_via);
        }
        session_set_state(g_db, session_id, "tool_running");
        free((char *)agent);
        approval_free(a);
        wake_session(session_id);
        run_advance(session_id);
        return;
    }

    int rename_failed = 0;
    if (decision == APPROVAL_ALWAYS)
        apply_grant(a, agent, &rename_failed);

    /* Build tool result message */
    char result_buf[256];
    if (rename_failed)
        snprintf(result_buf, sizeof(result_buf), "error: rename failed, rolled back");
    else if (decision == APPROVAL_ALWAYS)
        snprintf(result_buf, sizeof(result_buf), "approved: %s", a->action);
    else
        snprintf(result_buf, sizeof(result_buf), "denied (%s): %s",
                 decided_via, a->action);

    if (a->tool_call_id) {
        ToolResult tr = { .tool_call_id = a->tool_call_id, .content = result_buf };
        Message msg = { .role = ROLE_TOOL, .tool_result = &tr,
                        .tool_name = a->tool_name,
                        .is_error = (decision == APPROVAL_DENY || rename_failed) };
        entry_append_with_turn(g_db, session_id, &msg, 0);
        db_tool_call_set_status(g_db, session_id, a->tool_call_id, "done", decided_via);
    }

    session_set_state(g_db, session_id, "tool_running");
    free((char *)agent);
    approval_free(a);
    wake_session(session_id);
    run_advance(session_id);
}

/* ── handle_approval_park: prompt the approver ────────────────── */

static void handle_approval_park(int64_t session_id) {
    Approval *a = approval_get_pending(g_db, session_id);
    if (!a) return;

    if (g_mode == 0) {
        /* CLI mode */
        if (!isatty(STDIN_FILENO)) {
            /* Non-interactive (-p mode): auto-deny */
            resolve_approval(a->id, APPROVAL_DENY, "auto:no-approver");
            approval_free(a);
            return;
        }
        /* Interactive: prompt user */
        fprintf(stdout, "\n\033[1mApproval required:\033[0m %s", a->action);
        if (a->args_json) fprintf(stdout, " %s", a->args_json);
        if (a->resolve && strcmp(a->resolve, "apply") == 0)
            fprintf(stdout, "\nGrant? (y/n): ");
        else
            fprintf(stdout, "\nApprove? (y=always / o=once / n=no): ");
        fflush(stdout);
        g_cli_turn_active = 0;  /* unblock input loop for the y/n read */
        /* The actual y/n is read in the CLI input loop — see cli_handle_approval */
        approval_free(a);
        return;
    }

    /* Daemon mode: enqueue outbox prompt if session has channel, else auto-deny */
    const char *sql = "SELECT channel_name, channel_id FROM sessions WHERE id=?;";
    sqlite3_stmt *stmt;
    int has_channel = 0;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, session_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *ch = (const char *)sqlite3_column_text(stmt, 0);
            const char *cid = (const char *)sqlite3_column_text(stmt, 1);
            if (ch && ch[0]) {
                has_channel = 1;
                /* Enqueue approval prompt to channel */
                char prompt[512];
                snprintf(prompt, sizeof(prompt),
                         "Approval required: %s %s. Reply yes/no.",
                         a->action, a->args_json ? a->args_json : "");
                const char *ins_sql =
                    "INSERT INTO channel_outbox(channel_name, session_id, payload)"
                    " VALUES(?1, ?2, json_object('chat_id', ?3, 'text', ?4));";
                sqlite3_stmt *ins;
                if (sqlite3_prepare_v2(g_db, ins_sql, -1, &ins, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(ins, 1, ch, -1, SQLITE_STATIC);
                    sqlite3_bind_int64(ins, 2, session_id);
                    sqlite3_bind_text(ins, 3, cid ? cid : "0", -1, SQLITE_STATIC);
                    sqlite3_bind_text(ins, 4, prompt, -1, SQLITE_STATIC);
                    sqlite3_step(ins); sqlite3_finalize(ins);
                }
            }
        }
        sqlite3_finalize(stmt);
    }
    if (!has_channel) {
        /* No channel binding — fail-closed: auto-deny */
        resolve_approval(a->id, APPROVAL_DENY, "auto:no-approver");
    }
    approval_free(a);
}

/* ── run_advance: call advance_session and execute the decision ── */

/* ── event_step: shared event handling for both daemon and CLI loops ── */

static void reap_children(void);

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
            /* Only the root CLI session drives the prompt; sub-agents advance
             * silently in the background. */
            if (g_mode == 0 && session_id == g_cli_session) g_cli_turn_active = 0;
        }
        break;
    case ADVANCE_DISPATCH_TOOLS: {
        /* fork_tool_exec returns: 1 = inline (result written),
         * 3 = async parallel-safe dispatched (keep going), 0 = async serial
         * dispatched (stop and wait), 2 = parked for approval, <0 = failure.
         * Parallel-safe calls are launched back-to-back; a serial async call
         * (or a park/failure) stops dispatch and we wait for its completion. */
        /* The shared setup serves whichever session is advancing — root or any
         * sub-agent (in the daemon, many agents through one setup). Rebind caps
         * to the advancing session's agent before forking so each tool runs
         * under that agent's grants, not whoever dispatched last. */
        if (g_tool_setup)
            agent_setup_refresh_caps(g_tool_setup, g_db, out.agent_name);
        int async_in_flight = 0;
        int stop = 0;
        for (int i = 0; i < out.tc_count && !stop; i++) {
            int rc = fork_tool_exec(session_id, out.agent_name, &out.calls[i]);
            switch (rc) {
            case 1: break;                              /* inline done — next */
            case 3: async_in_flight = 1; break;         /* parallel async — next */
            case 0: async_in_flight = 1; stop = 1; break; /* serial async — wait */
            default: stop = 1; break;                   /* parked or failure */
            }
        }
        /* Only advance now if every call ran inline. If anything async is in
         * flight, its completion (reap or sub-agent finish) re-advances us. */
        if (!async_in_flight && !stop)
            run_advance(session_id);
        db_tool_call_free_pending(out.calls, out.tc_count);
        break;
    }
    case ADVANCE_DONE:
        /* Refresh only when the completing agent is the one the shared setup is
         * bound to (CLI root). A sub-agent completion must not leave root's
         * setup carrying the sub-agent's grants; the dispatch path re-binds
         * caps per call anyway. */
        if (g_tool_setup && strcmp(out.agent_name, g_agent_name) == 0)
            agent_setup_refresh_caps(g_tool_setup, g_db, out.agent_name);
        deliver_response(session_id);
        /* Attempt compaction if configured */
        if (g_cfg->compaction && llm_worker_alive() &&
            session_needs_compaction(g_db, session_id, g_cfg)) {
            session_set_state(g_db, session_id, "compacting");
            if (llm_worker_submit_compact(g_db, session_id, out.agent_name) != 0)
                session_set_state(g_db, session_id, "idle");
        }
        if (g_mode == 0 && session_id == g_cli_session) {
            g_cli_turn_active = 0;
        }
        break;
    case ADVANCE_WAITING:
    case ADVANCE_NOOP:
        /* Root turn stays active while its own async work (forked tools or
         * sub-agents) is in flight; awaiting_approval has neither, so the prompt
         * is released for the user's y/n. Sub-agents never touch the root flag. */
        if (g_mode == 0 && session_id == g_cli_session
            && !child_has_session(session_id)
            && !db_tool_call_any_running(g_db, session_id))
            g_cli_turn_active = 0;
        break;
    case ADVANCE_ERROR:
        if (g_mode == 0) {
            fprintf(stderr, "error: session advance failed\n");
            if (session_id == g_cli_session) g_cli_turn_active = 0;
        }
        break;
    }
}

static void deliver_response(int64_t session_id) {
    if (g_mode == 0) {
        /* CLI stdout belongs to the root session's turn. A sub-agent finishing
         * routes its result to the parent's tool_call (advance.c), not stdout. */
        if (session_id != g_cli_session) return;
        char *text = get_response_text(g_db, session_id);
        if (text) { printf("%s\n", text); free(text); }
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
            /* §8 observer hook (after execution; side-effect only) */
            if (g_tool_setup)
                hook_dispatch_observe_tool_call(&g_tool_setup->ext_ctx, g_db,
                                                c->tool_name, c->tool_args, output);
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
           "  --qjs_eval         sandboxed JS evaluator (forked child mode)\n"
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
        if (count >= cap) { cap *= 2; Row *tmp = realloc(rows, (size_t)cap * sizeof(Row)); if (!tmp) { free(rows); sqlite3_finalize(stmt); return -1; } rows = tmp; }
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

    /* Write channel.qjs */
    char js_path[2*PATH_MAX];
    snprintf(js_path, sizeof(js_path), "%s/channel.qjs", tg_dir);
    FILE *f = fopen(js_path, "w");
    if (f) { fputs(TPL_CHANNEL_TELEGRAM_QJS, f); fclose(f); }

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

/* ── --qjs_eval mode: sandboxed one-shot JS evaluator ────────────── */


#define QJS_EVAL_HEAP_SIZE (1024 * 1024)
#define QJS_EVAL_MAX_INSTRUCTIONS 10000000
#define QJS_EVAL_MAX_OUTPUT (60 * 1024)
#define QJS_EVAL_MAX_FILE  (1024 * 1024)

static const char *QJS_EVAL_PRELUDE =
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
    "  throw new TypeError('require() not available — there are no modules. Use globals: fs.readdir(path), fs.readFile(path), fs.writeFile(path, data), fs.stat(path), fs.cwd(), http_request(url).');\n"
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
    "var Set = function() { throw new TypeError('Set not available. Use: var s = {}; s[x] = true;'); };\n"
    "var print = console.log;\n";


/* The eval profile is ES2025 but models sometimes reach for patterns that
 * fail — provide actionable hints. */
static const char *qjs_syntax_hint(const char *code) {
    if (!code) return "";
    if (strstr(code, "const ") || strstr(code, "let "))
        return " — hint: this engine is ES5; use 'var' instead of 'const'/'let'";
    if (strstr(code, "=>"))
        return " — hint: arrow functions are unsupported; use function(x){ return ...; }";
    if (strstr(code, "`"))
        return " — hint: template literals are unsupported; concatenate with 'a' + b";
    if (strstr(code, "require(") || strstr(code, "import "))
        return " — hint: no modules; 'fs' and 'http_request' are globals (e.g. fs.readdir('.'))";
    if (strstr(code, "await ") || strstr(code, ".then("))
        return " — hint: this engine is synchronous; assign directly: var r = http_request(url); then use r.body / r.json()";
    return "";
}

/* Entry for the re-exec'd `cclaw --qjs_eval` JS child. main() jumps here before
 * any DB/config/log init (and before the daemon's civetweb ever starts), so the
 * JS sandbox process never initializes SQLite or civetweb — demand paging keeps
 * their text out of its RSS. No separate JS-only binary is required: the single
 * image is reused, and the unused subsystems simply stay non-resident. */
static int qjs_eval_main(int argc, char **argv) {
    /* (a) Parse argv after "--qjs_eval" */
    int inline_mode = 0;
    const char *code_str = NULL;
    const char *file_path = NULL;
    const char *args_json = NULL;

    if (argc < 3) {
        fprintf(stderr, "usage: cclaw --qjs_eval -e 'CODE' | FILE [ARGS_JSON]\n");
        _exit(1);
    }
    if (strcmp(argv[2], "-e") == 0) {
        if (argc < 4) { fprintf(stderr, "--qjs_eval -e requires code\n"); _exit(1); }
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
    const char *env_file_env = getenv("CCLAW_ENV_FILE");
    char *env_file = env_file_env ? strdup(env_file_env) : NULL;
    const char *proxy_env = getenv("CCLAW_PROXY_SOCK");
    char *proxy_sock = proxy_env ? strdup(proxy_env) : NULL;
    const char *host_mode_env = getenv("CCLAW_QJS_HOST");
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

    /* Layer 2: parse read/write paths from env → extra_mounts */
    size_t read_count = 0, write_count = 0;
    const char *rp_env = getenv("CCLAW_READ_PATHS");
    const char *wp_env = getenv("CCLAW_WRITE_PATHS");
    if (rp_env && rp_env[0]) {
        char *tmp = strdup(rp_env);
        for (char *t = strtok(tmp, ","); t; t = strtok(NULL, ",")) read_count++;
        free(tmp);
    }
    if (wp_env && wp_env[0]) {
        char *tmp = strdup(wp_env);
        for (char *t = strtok(tmp, ","); t; t = strtok(NULL, ",")) write_count++;
        free(tmp);
    }
    size_t n_extra = read_count + write_count;
    char **rp_strs = NULL, **wp_strs = NULL;

    /* Layer 5: always mount the shared extension store read-only so sandboxed
     * (non-host) agents can load promoted tool/hook handlers — which live under
     * <db_dir>/extensions — without an explicit read_path grant. */
    char store_dir[PATH_MAX] = {0};
    int have_store = 0;
    if (db_path && db_path[0]) {
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof(tmp), "%s", db_path);
        char *sl = strrchr(tmp, '/');
        if (sl) {
            *sl = '\0';
            snprintf(store_dir, sizeof(store_dir), "%s/extensions", tmp);
            struct stat sb;
            if (stat(store_dir, &sb) == 0 && S_ISDIR(sb.st_mode)) have_store = 1;
        }
    }

    /* (c) Sandbox — derive policy from trust level */
    const char *trust_env = getenv("CCLAW_TRUST_LEVEL");
    SandboxConfig cfg = {0};
    sandbox_policy_from_trust(trust_env, &cfg);
    cfg.workspace = workspace;
    cfg.db_path = db_path;
    cfg.env_file = env_file;
    cfg.cwd_path = NULL;
    cfg.proxy_sock = proxy_sock;
    if (no_sandbox) cfg.sandbox = 0;

    size_t total_extra = n_extra + (have_store ? 1 : 0);
    if (total_extra > 0) {
        cfg.extra_mounts = malloc(total_extra * sizeof(*cfg.extra_mounts));
        cfg.extra_mount_count = total_extra;
        size_t j = 0;
        if (read_count > 0) {
            rp_strs = malloc(read_count * sizeof(char *));
            char *tmp = strdup(rp_env);
            char *tok = strtok(tmp, ",");
            for (size_t i = 0; i < read_count; i++) {
                rp_strs[i] = strdup(tok);
                cfg.extra_mounts[j].path = rp_strs[i];
                cfg.extra_mounts[j].ro = 1;
                j++;
                tok = strtok(NULL, ",");
            }
            free(tmp);
        }
        if (write_count > 0) {
            wp_strs = malloc(write_count * sizeof(char *));
            char *tmp = strdup(wp_env);
            char *tok = strtok(tmp, ",");
            for (size_t i = 0; i < write_count; i++) {
                wp_strs[i] = strdup(tok);
                cfg.extra_mounts[j].path = wp_strs[i];
                cfg.extra_mounts[j].ro = 0;
                j++;
                tok = strtok(NULL, ",");
            }
            free(tmp);
        }
        if (have_store) {
            cfg.extra_mounts[j].path = store_dir;
            cfg.extra_mounts[j].ro = 1;
            j++;
        }
    }

    if (sandbox_child_setup(&cfg) != 0) {
        printf("error: sandbox setup failed\n");
        _exit(126);
    }

    /* (d) Create JS context with eval profile */
    QjsRuntime *qrt = qjs_runtime_create(QJS_EVAL_HEAP_SIZE);
    if (!qrt) { printf("error: out of memory\n"); _exit(1); }
    qjs_set_interrupt_limit(qrt, QJS_EVAL_MAX_INSTRUCTIONS);

    JSContext *ctx = qjs_context_create(qrt, QJS_PROFILE_EVAL);
    if (!ctx) { qjs_runtime_destroy(qrt); printf("error: JS context creation failed\n"); _exit(1); }

    /* Store allowed_hosts in context opaque for host functions */
    JsHostCtx hctx = {
        .instruction_count = 0,
        .instruction_limit = QJS_EVAL_MAX_INSTRUCTIONS,
        .allowed_hosts = allowed_hosts,
        .allowed_hosts_count = hosts_count,
    };
    JS_SetContextOpaque(ctx, &hctx);

    /* Register host C functions (http_request, fs.*, console, print) */
    qjs_register_eval_host_functions(ctx);

    /* (e) Run prelude */
    JSValue pv = JS_Eval(ctx, QJS_EVAL_PRELUDE, strlen(QJS_EVAL_PRELUDE), "<prelude>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(pv)) {
        char *msg = qjs_get_exception_string(ctx);
        printf("error: prelude failed: %s\n", msg ? msg : "unknown");
        free(msg);
        _exit(1);
    }
    JS_FreeValue(ctx, pv);

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
        if (sz < 0 || sz > QJS_EVAL_MAX_FILE) {
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
    JSValue val = JS_Eval(ctx, eval_code, eval_len, "<qjs_eval>", JS_EVAL_TYPE_GLOBAL);

    /* Drain microtasks and await/unwrap any returned promise (a rejection
     * re-throws, so it reports via the exception path below). */
    if (!JS_IsException(val))
        val = qjs_resolve(ctx, val);

    int failed = 0;
    char *result = NULL;

    if (JS_IsException(val)) {
        failed = 1;
        char *msg = qjs_get_exception_string(ctx);
        if (msg) {
            const char *hint = strstr(msg, "SyntaxError") ? qjs_syntax_hint(eval_code) : "";
            size_t len = strlen(msg) + strlen(hint) + 16;
            result = malloc(len);
            if (result) snprintf(result, len, "error: %s%s", msg, hint);
            else result = strdup("error: OOM");
            free(msg);
        } else {
            result = strdup("error: exception (no message)");
        }
    } else if (JS_IsUndefined(val)) {
        /* (h) Console fallback */
        JS_FreeValue(ctx, val);
        const char *check = "__console_buf.length > 0 ? __console_buf.join('\\n') : undefined";
        JSValue buf_val = JS_Eval(ctx, check, strlen(check), "<console>", JS_EVAL_TYPE_GLOBAL);
        if (!JS_IsUndefined(buf_val) && !JS_IsException(buf_val)) {
            const char *str = JS_ToCString(ctx, buf_val);
            result = str ? strdup(str) : strdup("undefined");
            if (str) JS_FreeCString(ctx, str);
        } else {
            result = strdup("undefined");
        }
        JS_FreeValue(ctx, buf_val);
    } else if (JS_IsNull(val)) {
        JS_FreeValue(ctx, val);
        result = strdup("null");
    } else {
        const char *str = JS_ToCString(ctx, val);
        result = str ? strdup(str) : strdup("error: cannot convert result to string");
        if (str) JS_FreeCString(ctx, str);
        JS_FreeValue(ctx, val);
    }

    /* (i) Output */
    size_t out_len = strlen(result);
    if (out_len > QJS_EVAL_MAX_OUTPUT) out_len = QJS_EVAL_MAX_OUTPUT;
    fwrite(result, 1, out_len, stdout);
    if (out_len > 0 && result[out_len - 1] != '\n') fwrite("\n", 1, 1, stdout);
    fflush(stdout);

    /* Cleanup */
    free(result);
    if (!inline_mode && eval_code != code_str) free(eval_code);
    JS_FreeContext(ctx);
    qjs_runtime_destroy(qrt);
    for (size_t i = 0; i < hosts_count; i++) free(allowed_hosts[i]);
    free(allowed_hosts);
    free(workspace);
    free(db_path);
    free(env_file);
    free(proxy_sock);

    _exit(failed ? 1 : 0);
}

static int run_daemon(char *db_path) {
    g_mode = 1;
    g_next_db_poll = time(NULL);  /* run DB checks immediately on first iter */
    workspace_init(g_cfg);

    /* Tool dispatch setup. Without this g_tool_setup is NULL and every forkable
     * tool resolves to "unknown tool" in daemon mode. The daemon serves many
     * agents through this one shared setup; caps are re-bound to the advancing
     * session's agent before each dispatch (run_advance). The init agent name
     * just seeds the caps/contexts; agents_dir backs rename support. */
    char daemon_agent[64];
    { char *def = db_kv_get(g_db, "default_agent");
      snprintf(daemon_agent, sizeof(daemon_agent), "%s", def ? def : "default");
      free(def); }
    snprintf(g_agent_name, sizeof(g_agent_name), "%s", daemon_agent);
    char daemon_agents_dir[PATH_MAX];
    { char base[PATH_MAX]; snprintf(base, sizeof(base), "%s", db_path);
      char *sl = strrchr(base, '/'); if (sl) *sl = '\0'; else snprintf(base, sizeof(base), ".");
      snprintf(daemon_agents_dir, sizeof(daemon_agents_dir), "%s/agents", base); }
    AgentSetup daemon_setup;
    agent_setup_init(&daemon_setup, g_db, 0, g_cfg, g_agent_name, AGENT_SETUP_DAEMON);
    daemon_setup.req_cfg_ctx.agents_dir = daemon_agents_dir;
    daemon_setup.req_cfg_ctx.agent_name = g_agent_name;
    g_tool_setup = &daemon_setup;

    printf("cclaw %s — daemon mode\n", CCLAW_VERSION);
    web_start(g_cfg, g_db, db_path);
    heartbeat_start(g_cfg, g_db);

    /* Start LLM worker threads */
    if (llm_worker_start(db_path, g_llm_threads) != 0) {
        fprintf(stderr, "error: failed to start LLM worker\n");
        config_free(g_cfg); db_close(g_db); free(db_path); return 1;
    }
    int daemon_worker_fd = llm_worker_fd();
    set_nonblock(daemon_worker_fd);

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

    /* Register in the liveness table so this daemon's in-flight sessions are
     * owner-stamped and never reclaimed by a peer's recovery. */
    if (process_register(g_db, "daemon", getpid(), g_instance_id, sizeof(g_instance_id)) != 0)
        fprintf(stderr, "warning: process registration failed\n");
    db_set_instance_id(g_instance_id);

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

        int rc = poll(pfds, (nfds_t)nfds, compute_timeout_ms());
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

        child_sweep_deadlines();
        channel_tick(g_db);

        /* DB periodic work — gated on interval */
        time_t now = time(NULL);
        if (now >= g_next_db_poll) {
            db_periodic();
            g_next_db_poll = now + POLL_DB_INTERVAL;
        }
    }

    free(pfds);

    /* Shutdown */
    llm_worker_stop();
    channel_shutdown_all();
    heartbeat_stop(); web_stop();
    /* Disarm SIGCHLD before closing the self-pipe: a child reaped after the
     * close would otherwise have the handler write into a reused fd. */
    signal(SIGCHLD, SIG_DFL);
    close(g_chld_pipe[0]); close(g_chld_pipe[1]);
    wake_close(); wake_fifo_close(fifo_fd, db_path);
    process_unregister(g_db, g_instance_id);
    g_tool_setup = NULL;
    agent_setup_destroy(&daemon_setup);
    config_free(g_cfg); db_close(g_db); free(db_path);
    return 0;
}

static int run_cli(char *db_path, const char *prompt,
                   int64_t session_id, int new_session, int host_mode) {
    /* ── CLI mode ────────────────────────────────────────────────── */
    g_mode = 0;
    g_next_db_poll = time(NULL);

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
          /* Create default agent */
          const char *agent_sql =
              "INSERT OR IGNORE INTO agents(name, system_prompt, trust_level)"
              " VALUES('default', ?, 'trusted');"
              ;
          sqlite3_stmt *bs;
          if (sqlite3_prepare_v2(g_db, agent_sql, -1, &bs, NULL) == SQLITE_OK) {
              sqlite3_bind_text(bs, 1, TPL_DEFAULT_SYSTEM_PROMPT_MD, -1, SQLITE_STATIC);
              sqlite3_step(bs); sqlite3_finalize(bs);
          }
          /* Seed default tools as grants */
          agent_grant_defaults(g_db, "default");
          db_kv_set(g_db, "default_agent", "default");
          /* Seed default memory blocks */
          memory_block_create(g_db, "default", "AGENT",
              "Your identity, capabilities, and operational notes. Update as you learn about yourself.",
              NULL, 5000);
          memory_block_create(g_db, "default", "USER",
              "Information about the user: preferences, context, working style. Update as you learn.",
              NULL, 5000);
          memory_entry_add(g_db, "default", "AGENT",
              "You are CClaw. You do not have a name yet — ask the user what they would like to call you, then save it here with memory_edit.");
          memory_entry_add(g_db, "default", "USER",
              "Record what you learn about the user here: their name, preferences, and how they like you to work.");
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

    /* Inject agent config env vars (for forked children) */
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
    setenv("CCLAW_MODE", "cli", 1);
    workspace_init(g_cfg);
    /* Make the workspace the process cwd so relative paths, shell children, and
     * fs.* all operate in the agent's workspace by default. (CLI only — the
     * daemon serves multiple agents and its tool children chdir per-agent.) */
    if (g_cfg->workspace && chdir(g_cfg->workspace) != 0)
        fprintf(stderr, "warning: chdir to workspace %s failed: %s\n",
                g_cfg->workspace, strerror(errno));
    { char cwd[PATH_MAX]; if (getcwd(cwd, sizeof(cwd))) setenv("CCLAW_PATH", cwd, 1); }

    /* Set up tool schemas env for LLM proc children */
    AgentSetup setup;
    agent_setup_init(&setup, g_db, 0, g_cfg, g_agent_name, AGENT_SETUP_CLI);
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

    /* Register in the liveness table before touching session state, so our
     * transitions stamp owner_instance and recovery won't reclaim them. */
    if (process_register(g_db, "cli", getpid(), g_instance_id, sizeof(g_instance_id)) != 0)
        fprintf(stderr, "warning: process registration failed\n");
    db_set_instance_id(g_instance_id);

    /* Refuse to drive a session another live process is mid-turn on — two
     * writers would corrupt its branch. Idle (owner NULL) or dead-owned
     * sessions are takeable; only a live *other* owner blocks us. */
    {
        char st[32] = {0}, owner[40] = {0};
        sqlite3_stmt *gs;
        if (sqlite3_prepare_v2(g_db,
                "SELECT state, COALESCE(owner_instance,'') FROM sessions WHERE id=?;",
                -1, &gs, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(gs, 1, session_id);
            if (sqlite3_step(gs) == SQLITE_ROW) {
                snprintf(st, sizeof st, "%s", (const char *)sqlite3_column_text(gs, 0));
                snprintf(owner, sizeof owner, "%s", (const char *)sqlite3_column_text(gs, 1));
            }
            sqlite3_finalize(gs);
        }
        int transient = strcmp(st, "idle") != 0 && st[0];
        if (transient && owner[0] && strcmp(owner, g_instance_id) != 0 &&
            process_is_live(g_db, owner, PROCESS_TTL_SEC)) {
            char omode[16] = "cclaw"; int opid = 0;
            sqlite3_stmt *ps;
            if (sqlite3_prepare_v2(g_db,
                    "SELECT mode, pid FROM processes WHERE instance_id=?;",
                    -1, &ps, NULL) == SQLITE_OK) {
                sqlite3_bind_text(ps, 1, owner, -1, SQLITE_STATIC);
                if (sqlite3_step(ps) == SQLITE_ROW) {
                    const char *m = (const char *)sqlite3_column_text(ps, 0);
                    if (m) snprintf(omode, sizeof omode, "%s", m);
                    opid = sqlite3_column_int(ps, 1);
                }
                sqlite3_finalize(ps);
            }
            fprintf(stderr,
                    "error: session %lld is being driven by %s pid %d\n",
                    (long long)session_id, omode, opid);
            process_unregister(g_db, g_instance_id);
            agent_setup_destroy(&setup); free(base_dir);
            config_free(g_cfg); db_close(g_db); free(db_path);
            return 1;
        }
    }

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

    /* Wake pipe: sub-agent launch and completion wake sessions through this fd,
     * exactly as in the daemon loop. Without it wake_session() is a no-op, so a
     * spawned child session would never advance and a parent waiting on it would
     * hang. This is what lets the CLI run the multi-agent code, not just the
     * daemon — the only remaining difference is that the CLI has no channels. */
    int wake_pipe_fd = -1;
    if (wake_init() == 0) wake_pipe_fd = wake_fd();

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

        int wake_idx = -1;
        if (wake_pipe_fd >= 0) {
            wake_idx = cli_nfds;
            cli_pfds[cli_nfds].fd = wake_pipe_fd; cli_pfds[cli_nfds].events = POLLIN; cli_nfds++;
        }

        int result_pipe_base = cli_nfds;
        cli_nfds = add_result_pipe_fds(cli_pfds, cli_nfds, cli_max_pfds);

        int nready = poll(cli_pfds, (nfds_t)cli_nfds, compute_timeout_ms());
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
        if (wake_idx >= 0 && (cli_pfds[wake_idx].revents & POLLIN)) {
            /* Sub-agent sessions woken by launch/completion advance here. */
            WakeMsg msg;
            while (read(wake_pipe_fd, &msg, sizeof(msg)) == (ssize_t)sizeof(msg)) {
                if (child_has_session(msg.session_id)) continue;
                run_advance(msg.session_id);
            }
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

            /* Check if we're waiting for an approval decision */
            {
                Approval *pa = approval_get_pending(g_db, g_cli_session);
                if (pa) {
                    ApprovalDecision d;
                    if (strcasecmp(linebuf, "once") == 0 || strcasecmp(linebuf, "o") == 0)
                        d = APPROVAL_ONCE;
                    else if (strcasecmp(linebuf, "y") == 0 ||
                             strcasecmp(linebuf, "yes") == 0 ||
                             strcasecmp(linebuf, "ok") == 0 ||
                             strcasecmp(linebuf, "okay") == 0 ||
                             strcasecmp(linebuf, "approve") == 0)
                        d = APPROVAL_ALWAYS;
                    else
                        d = APPROVAL_DENY;
                    resolve_approval(pa->id, d, "cli:interactive");
                    approval_free(pa);
                    /* Shift and continue */
                    size_t consumed = (size_t)(nl - linebuf) + 1;
                    linepos -= consumed;
                    if (linepos > 0) memmove(linebuf, nl + 1, linepos);
                    continue;
                }
            }

            cli_start_turn(linebuf);
            /* Shift any remaining data after the newline */
            size_t consumed = (size_t)(nl - linebuf) + 1;
            linepos -= consumed;
            if (linepos > 0) memmove(linebuf, nl + 1, linepos);
        }

        child_sweep_deadlines();

        /* DB periodic work — gated on interval */
        time_t now = time(NULL);
        if (now >= g_next_db_poll) {
            db_periodic();
            g_next_db_poll = now + POLL_DB_INTERVAL;
        }
    }

done:
    /* Print session cost */
    { int64_t cost = session_cost(g_db, g_cli_session);
      if (cost > 0) fprintf(stderr, "\n[session cost: $%.6f]\n", (double)cost / 1e9); }

    session_set_state(g_db, g_cli_session, "idle");
    /* Quiesce the worker pool before freeing anything it may still touch
     * (g_tool_setup/g_cfg/g_db). Mirrors the daemon shutdown order. */
    llm_worker_stop();
    agent_setup_destroy(&setup);
    /* Disarm SIGCHLD before closing the self-pipe: a child reaped after the
     * close would otherwise have the handler write into a reused fd. */
    signal(SIGCHLD, SIG_DFL);
    close(g_chld_pipe[0]); close(g_chld_pipe[1]);
    wake_close();
    free(cli_pfds);
    /* Drop our registry row, then run recovery: any still-transient sessions we
     * owned (e.g. -p exiting with background sub-agents in flight, or a turn cut
     * short) are now dead-owned and get reclaimed instead of orphaned. */
    process_unregister(g_db, g_instance_id);
    db_recover_stale_sessions(g_db);
    free(base_dir); config_free(g_cfg); db_close(g_db); free(db_path);
    return rc;
}

int main(int argc, char *argv[]) {
    /* --qjs_eval: early intercept before any config/logging setup */
    if (argc >= 2 && strcmp(argv[1], "--qjs_eval") == 0) return qjs_eval_main(argc, argv);
    /* --run-tool: early intercept for sandboxed file tool child. No DB, no key,
     * no config. The child reads its request from fd 3. */
    if (argc >= 2 && strcmp(argv[1], "--run-tool") == 0) return run_tool_main();

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
    if (g_cfg->env_file) setenv("CCLAW_ENV_FILE", g_cfg->env_file, 1);
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
    /* The daemon fork+execs `cclaw --channel <name>` (do_fork) for a clean
     * process image; we run the channel loop directly here — no separate
     * channel_runner binary, so ps shows `cclaw --channel <name>`. The runner
     * opens its own DB ctx, so drop ours first. */
    if (channel_mode) {
        config_free(g_cfg); db_close(g_db);
        int rc = channel_runner_main(db_path, channel_mode);
        free(db_path);
        return rc;
    }

    /* ── Startup crash recovery (covers both CLI and daemon). GC dead registry
     * rows first so crashed predecessors are absent from the processes table,
     * making their sessions dead-owned and thus reclaimable. This process is not
     * yet registered, so it owns nothing — a live peer's row spares its own
     * sessions. Owner-scoped, so it's also safe on the periodic path. ── */
    process_gc_dead(g_db, PROCESS_TTL_SEC);
    if (db_recover_stale_sessions(g_db) != 0)
        fprintf(stderr, "warning: startup recovery failed\n");

    if (daemon_mode) return run_daemon(db_path);
    return run_cli(db_path, prompt, session_id, new_session, host_mode);
}
