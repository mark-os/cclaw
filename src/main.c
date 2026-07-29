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
#include <time.h>
#include <unistd.h>

#include "cclaw.h"
#include "config.h"
#include "config_registry.h"
#include "dashboard.h"
#include "log.h"
#include "sandbox.h"
#include "tool_js.h"
#include "qjs_helpers.h"
#include "agent_config.h"
#include "agent_define.h"
#include "skills.h"
#include "agent_setup.h"
#include "hook_dispatch.h"
#include "approval.h"
#include "buf.h"
#include "llm_proc.h"
#include "llm_worker.h"
#include "tool_thread.h"
#include "tools.h"
#include "tool_args.h"
#include "tool_request_config.h"
#include "context.h"
#include "db.h"
#include "secret_store.h"
#include "cli_verbs.h"
#include "templates.h"
#include "db_response.h"
#include "shutdown.h"
#include "crash.h"
#include "channel_harness.h"
#include "extension_manifest.h"
#include "doctor.h"
#include "install.h"
#include "wake.h"
#include "advance.h"
#include "channel.h"
#include "channel_api.h"
#include "channel_runner.h"
#include "secret.h"
#include "validate.h"
#include "secret_scan.h"
#include "tool_policy.h"
#include "secret_interp.h"
#include "secret_capture.h"
#include "external_content.h"
#include "unicode_normalize.h"
#include "run_tool.h"
#include "tool_file.h"
#include "resolve.h"
#include "web.h"
#include "cron.h"
#include "util.h"

_Static_assert(sizeof(WakeMsg) <= PIPE_BUF,
    "WakeMsg must fit in PIPE_BUF so wake-pipe writes stay atomic");

/* ── Constants ──────────────────────────────────────────────────── */

#define CHILD_MAX 48
#define TOOL_MAX_OUTPUT (60 * 1024)  /* 60KB — fits in pipe buffer */

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
    /* fd-3 frame parse state: [4-byte meta_len (network order)][hosts JSON]
     * [result to EOF]. The pipe is non-blocking and poll-driven, so header
     * and meta can arrive fragmented across drains. */
    size_t frame_hdr_read;      /* bytes of the 4-byte header consumed */
    unsigned char frame_hdr[4];
    size_t frame_meta_len;      /* parsed meta length (valid once hdr_read==4) */
    size_t frame_meta_read;     /* meta bytes consumed so far */
    char *hosts_json;           /* meta payload; NULL if absent/oversized */
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
    if (c->result_pipe >= 0) {
        close(c->result_pipe);
        c->result_pipe = -1;
    }
    free(c->outbuf);
    c->outbuf = NULL;
    c->outbuf_len = 0;
    free(c->hosts_json);
    c->hosts_json = NULL;
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

/* Sessions whose tool dispatch hit the CHILD_MAX ceiling (dispatch_tool
 * returned -1). They sit in tool_running with pending, un-forked tool_calls —
 * nothing was forked, so no reap re-advances them and they'd hang until owner-
 * death recovery. When a tool slot frees (reap), re-advance them: advance_session
 * re-reads the still-'pending' calls and re-dispatches (idempotent — dispatch_
 * tool_inner bails at the ceiling check before touching call state). Ephemeral
 * scheduling hint, never a source of truth; db_recover_stale_sessions is the
 * durable backstop across a daemon restart. */
static int64_t g_stalled[CHILD_MAX];
static int g_stalled_count;

static void stalled_add(int64_t session_id) {
    for (int i = 0; i < g_stalled_count; i++)
        if (g_stalled[i] == session_id) return;   /* dedup */
    if (g_stalled_count < CHILD_MAX)
        g_stalled[g_stalled_count++] = session_id;
}

/* ── Globals ────────────────────────────────────────────────────── */

/* Forward declarations */
static void deliver_response(int64_t session_id);
static int dispatch_tool(int64_t session_id, const char *agent_name, PendingToolCall *tc);
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
static int g_auto_approve;      /* 1 = --auto-approve: answer parks without prompting (CLI only) */

/* SIGCHLD self-pipe */
static int g_chld_pipe[2] = {-1, -1};

static void sigchld_handler(int sig) {
    (void)sig;
    char c = 1;
    (void)write(g_chld_pipe[1], &c, 1);
}

/* ── dispatch_llm_req ────────────────────────────────────────────── */

static AgentSetup *g_tool_setup;  /* Initialized once for tool dispatch */

/* Dispatch LLM request via worker thread pool */
/* Effective iteration cap for a session: agents.max_iterations (if > 0)
 * overrides global config. run_advance and the dispatch_llm_req fallback must
 * resolve this identically — a global-only fallback caps a raised per-agent
 * limit and kills the turn early. */
static int session_max_iter(int64_t session_id) {
    int max_iter = g_cfg->max_iterations > 0 ? g_cfg->max_iterations
                                              : config_default_int("max_iterations");
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(g_db,
            "SELECT a.max_iterations FROM agents a"
            " JOIN sessions s ON s.agent_name = a.name"
            " WHERE s.id = ?", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, session_id);
        if (sqlite3_step(s) == SQLITE_ROW) {
            int ami = sqlite3_column_int(s, 0);
            if (ami > 0) max_iter = ami;
        }
        sqlite3_finalize(s);
    }
    return max_iter;
}

/* One-shot user notice for a deferred (not failed) turn — rate limit or cost
 * ceiling. No assistant entry: the leaf must stay the user entry so
 * session_sweep_inbox keeps retrying until the window clears. Daemon mode
 * posts to the session's channel; CLI prints and releases the prompt. */
static void notify_deferred(int64_t session_id, const char *text) {
    if (g_mode == 0) {
        fprintf(stderr, "\n%s\n", text);
        if (session_id == g_cli_session) g_cli_turn_active = 0;
        return;
    }
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(g_db,
            "INSERT INTO channel_outbox(channel_name, session_id, payload)"
            " SELECT channel_name, id,"
            "        json_object('chat_id', COALESCE(chat_id,'0'), 'text', ?2)"
            "   FROM sessions WHERE id=?1 AND channel_name IS NOT NULL;",
            -1, &s, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(s, 1, session_id);
    sqlite3_bind_text(s, 2, text, -1, SQLITE_STATIC);
    sqlite3_step(s);
    sqlite3_finalize(s);
    if (sqlite3_changes(g_db) > 0 && g_cfg && g_cfg->db_path) {
        char *ch = db_scalar_text(g_db,
            "SELECT channel_name FROM sessions WHERE id=?1;", session_id);
        if (ch) { channel_outbox_wake(g_cfg->db_path, ch); free(ch); }
    }
}

static int dispatch_llm_req(int64_t session_id, const char *agent_name, int iteration) {
    if (child_has_session(session_id)) return -1;

    /* Turn start: move queued inbox into entries. consumed>0 marks a fresh
     * user message (sweep retries of a parked session consume nothing) —
     * the dedup signal for the one-shot deferral notices below. */
    int consumed = 0;
    if (iteration == 0)
        consumed = inbox_consume_into_entries(g_db, session_id, 100);

    if (!llm_worker_alive()) return -1;

    if (iteration >= session_max_iter(session_id)) {
        /* The advance_session check normally catches this; this is a fallback.
         * Use a simple message — the main path in advance.c builds the rich one. */
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
    if (!rate_limit_check(g_db, g_cfg->token_rate_limit)) {
        LOG_WARN_("token_rate_limit hit, session %lld rate_limited",
                  (long long)session_id);
        session_set_state(g_db, session_id, "rate_limited");
        if (consumed > 0)
            notify_deferred(session_id,
                "rate limited: hourly token cap reached — your message is"
                " queued and will be answered when the limit clears");
        return -1;
    }

    /* Daily cost ceiling — rolling 24h. Unpriced models record cost 0, so the
     * token cap above stays the always-available brake. */
    if (g_cfg->daily_cost_limit_nano > 0) {
        int64_t spent = db_cost_last_24h(g_db);
        if (spent >= g_cfg->daily_cost_limit_nano) {
            LOG_WARN_("daily_cost_limit hit spent_nano=%lld limit_nano=%lld,"
                      " session %lld rate_limited",
                      (long long)spent, (long long)g_cfg->daily_cost_limit_nano,
                      (long long)session_id);
            session_set_state(g_db, session_id, "rate_limited");
            if (consumed > 0)
                notify_deferred(session_id,
                    "budget limit: daily cost ceiling reached — your message is"
                    " queued and will be answered when spend drops below it");
            return -1;
        }
    }

    /* Disk floor: an LLM turn writes a full response body + entries + archive.
     * Refuse before that write when free space is under the floor, so a full
     * disk degrades loudly instead of corrupting. Revert to idle like a
     * rejected worker-submit — a later poke retries once space frees. */
    int disk_floor_mb = config_default_int("disk_min_free_mb");
    if (disk_floor_mb > 0) {
        long free_mb = db_free_mb(g_db);
        if (free_mb >= 0 && free_mb < disk_floor_mb) {
            LOG_WARN_("disk_low free_mb=%ld floor_mb=%d deferring llm dispatch",
                      free_mb, disk_floor_mb);
            session_set_state(g_db, session_id, "idle");
            return -1;
        }
    }

    /* preAdvance hooks: run main-thread after the max-iter and rate-limit
     * gates (never fire for an unsent request); their commands cross to the
     * worker's payload build as DB state (hook_directives / entries.data). A
     * worker-submit failure below leaves directives behind for the retried
     * request — acceptable, llm_req clears them on every exit. */
    if (g_tool_setup) {
        char *cmds = hook_dispatch_pre_advance(&g_tool_setup->ext_ctx, g_db, session_id);
        if (cmds) {
            hook_apply_pre_advance(g_db, session_id, cmds);
            free(cmds);
        }
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

/* Forward declarations for approval + advance (used by dispatch_tool and resolve_approval) */
static void run_advance(int64_t session_id);
static void handle_approval_park(int64_t session_id);
/* resolve_approval declared in resolve.h (non-static) */

/* Re-advance sessions stalled on the child ceiling. Called after a tool slot
 * frees (reap). Snapshot-and-clear first: run_advance may re-stall a session
 * if the ceiling is still hit, re-populating the set for the next reap. */
static void stalled_drain(void) {
    if (g_stalled_count == 0) return;
    int64_t snap[CHILD_MAX];
    int n = g_stalled_count;
    memcpy(snap, g_stalled, (size_t)n * sizeof(*snap));
    g_stalled_count = 0;
    for (int i = 0; i < n; i++)
        run_advance(snap[i]);
}

/* ── dispatch_tool ─────────────────────────────────────────────── */

/* NULL-result handling for EXEC_INLINE tools (see dispatch_inline): a NULL
 * return means the handler dispatched async work instead of finishing inline.
 * NULL_NONE   — not actually async; NULL is an error (default: "returned null")
 * NULL_ASYNC  — sub-agent launched; keep going per parallel_safe (launch_agent)
 * NULL_PARK   — approval gate; session parks awaiting_approval */
typedef enum { NULL_NONE = 0, NULL_ASYNC, NULL_PARK } ToolNullKind;

/* Per-tool traits, collapsed from three separate strcmp chains. Tools not
 * listed get the defaults: serial, no interpolation, NULL is an error. */
typedef struct {
    const char *name;
    int parallel_safe;   /* sibling calls in one turn may run concurrently */
    int needs_interp;    /* {{SECRET:X}} resolved to real values at exec time */
    ToolNullKind null_kind;
} ToolTraits;

static const ToolTraits tool_traits[] = {
    /* Default is serial (safe): models emit ordered calls — shell especially —
     * expecting sequential side effects on the shared workspace. Only
     * independent, self-contained delegations opt in. */
    { "launch_agent",      1, 0, NULL_ASYNC },
    { "shell_exec",        0, 1, NULL_NONE },
    { "web_fetch",         0, 1, NULL_NONE },
    { "js_eval",           0, 1, NULL_NONE },
    { "request_config",    0, 0, NULL_PARK },
    { "create_agent",      0, 0, NULL_PARK },
    { "update_agent",      0, 0, NULL_PARK },
    { "extension_promote", 0, 0, NULL_PARK },
};

static const ToolTraits *tool_traits_lookup(const char *name) {
    for (size_t i = 0; i < sizeof(tool_traits) / sizeof(tool_traits[0]); i++)
        if (strcmp(tool_traits[i].name, name) == 0) return &tool_traits[i];
    return NULL;
}

static int tool_is_parallel_safe(const char *name) {
    const ToolTraits *t = tool_traits_lookup(name);
    return t ? t->parallel_safe : 0;
}

static int tool_needs_interpolation(const char *name) {
    const ToolTraits *t = tool_traits_lookup(name);
    return t ? t->needs_interp : 0;
}

/* ── CLI terminal output ─────────────────────────────────────────────
 * The CLI runs in the terminal's canonical mode: the kernel echoes input
 * and owns line editing, so the prompt below is never deletable and we
 * never enter raw mode. Output here is the program's UI (stdout), distinct
 * from diagnostics (syslog via LOG_*). */

#define CLI_PROMPT "> "   /* set to "" to drop the prompt symbol */

/* ANSI only when stdout is a real terminal — piped / -p output stays clean. */
static int cli_ansi(void) {
    static int v = -1;
    if (v < 0) v = isatty(STDOUT_FILENO) ? 1 : 0;
    return v;
}

static void cli_prompt(void) {
    fputs(CLI_PROMPT, stdout);
    fflush(stdout);
}

/* Transient "working" cue shown between the user's Enter and the turn's first
 * output. The response isn't streamed, so a tool-less turn would otherwise
 * show nothing while the model runs; the first real output clears it. */
static int g_cli_indicator;
static void cli_indicator_show(void) {
    if (!cli_ansi()) return;
    fputs("\033[2m…\033[0m", stdout);
    fflush(stdout);
    g_cli_indicator = 1;
}
static void cli_indicator_clear(void) {
    if (g_cli_indicator && cli_ansi()) { fputs("\r\033[K", stdout); fflush(stdout); }
    g_cli_indicator = 0;
}

/* CLI progress: "[tool_name {"arg":"value"}]" dimmed, args truncated */
static void cli_print_tool_call(const char *name, const char *args) {
    cli_indicator_clear();
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

/* Placeholder names in args that reference a LOADED secret. Unknown names
 * interpolate to nothing (nothing is submitted) — not checked or narrowed. */
static char **used_secret_names(const char *args, const ShellSecret *secrets,
                                size_t secret_count, size_t *out_n) {
    *out_n = 0;
    if (secret_count == 0) return NULL;
    size_t all_n = 0;
    char **all = secret_placeholder_names(args, &all_n);
    if (!all) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < all_n; i++) {
        int loaded = 0;
        for (size_t j = 0; j < secret_count; j++)
            if (strcmp(all[i], secrets[j].name) == 0) { loaded = 1; break; }
        if (loaded) all[k++] = all[i]; else free(all[i]);
    }
    *out_n = k;
    if (k == 0) { free(all); return NULL; }
    return all;
}

/* Host of a call's "url" argument: scheme-strip + authority slice, mirroring
 * what the proxy/http layer accepts — deliberately not a full URL parser. */
static int web_args_url_host(const char *args, char *out, size_t cap) {
    char *url = tool_args_str(g_db, args, "url");
    int ok = 0;
    if (url) {
        const char *p = strstr(url, "://");
        p = p ? p + 3 : url;
        const char *end = p + strcspn(p, "/?#");
        const char *at = memchr(p, '@', (size_t)(end - p));
        if (at) p = at + 1;
        if (*p == '[') {  /* v6 literal: [::1]:port */
            const char *rb = memchr(p, ']', (size_t)(end - p));
            if (rb) { p++; end = rb; }
        } else {
            const char *colon = memchr(p, ':', (size_t)(end - p));
            if (colon) end = colon;
        }
        size_t len = (size_t)(end - p);
        if (len > 0 && len < cap) { memcpy(out, p, len); out[len] = '\0'; ok = 1; }
    }
    free(url);
    return ok;
}

/* Interpolate {{SECRET:name}} into extracted wire-param values (TEXT/JSON
 * kinds; LIST params are file_edit's — never secret-bearing). After this the
 * array carries plaintext: free with tool_wire_args_wipe_free. */
static void wire_params_interpolate(ToolWireArg *params, size_t n,
                                    const ShellSecret *secrets, size_t count) {
    if (!params || count == 0) return;
    for (size_t i = 0; i < n; i++) {
        if (params[i].kind == TOOL_ARG_LIST || !params[i].value) continue;
        char *ip = secret_interpolate(params[i].value, secrets, count);
        if (ip) { free(params[i].value); params[i].value = ip; }
    }
}

/* Per-call egress state for one network-tier dispatch (specs/trust.md):
 *  - deny: sensitivity labels for req.deny_rules, minus labels covering an
 *    approved sensitive host (per-call exception, label-wide for exactly one
 *    human-approved call, gone when the call ends).
 *  - declared hosts (Q1): a *promoted* tool whose manifest declares hosts
 *    reaches exactly those, and the declaration REPLACES the agent's host
 *    grants for this call — no agent grant needed, and the agent's grants do
 *    not widen it. A tool that declares none (every builtin, every draft —
 *    drafts have no `tools` row at all) runs under the agent's grants.
 *  - credential narrowing: a call whose args carry loaded secrets talks ONLY
 *    to the union of those secrets' bound hosts. Fail-closed: an unbound
 *    secret yields an empty allow list (deny-all); the only way past is a
 *    human approval of this exact frozen call (skip_bind). */
typedef struct {
    char **deny; size_t deny_n;          /* owned labels for req.deny_rules */
    char **bound; size_t bound_n;        /* owned union of bound hosts */
    char **decl; size_t decl_n;          /* owned declared hosts (promoted tool) */
    int narrowed;
    const char **ext; size_t ext_n;      /* owned array, borrowed strings */
    const char **hosts; size_t hosts_n;  /* what the req should carry */
} CallEgress;

static void call_egress_build(CallEgress *ce, const char *tool_name,
                              const char *args, int skip_bind,
                              const char *sens_exception,
                              char **base_hosts, size_t base_n,
                              const ShellSecret *secrets, size_t secret_count) {
    memset(ce, 0, sizeof(*ce));
    int n = 0;
    char **all = db_sensitive_hosts(g_db, &n);
    if (all) {
        size_t k = 0;
        for (int i = 0; i < n; i++) {
            if (sens_exception && sens_exception[0] &&
                host_covered(&all[i], 1, sens_exception)) { free(all[i]); continue; }
            all[k++] = all[i];
        }
        ce->deny = all; ce->deny_n = k;
        if (k == 0) { free(all); ce->deny = NULL; }
    }

    /* Declared hosts replace the agent's grants — not intersect them, so a
     * declared-hosts tool needs no grant, and not union them, so a
     * declaration could never widen what the agent already holds. */
    int dn = 0;
    ce->decl = db_tool_declared_hosts(g_db, tool_name, &dn);
    ce->decl_n = (size_t)dn;

    const char **hosts = ce->decl ? (const char **)ce->decl
                                  : (const char **)base_hosts;
    size_t hosts_n = ce->decl ? ce->decl_n : base_n;
    if (!skip_bind && args) {
        size_t un = 0;
        char **names = used_secret_names(args, secrets, secret_count, &un);
        if (names) {
            char **u = NULL; size_t uc = 0, ucap = 0;
            for (size_t i = 0; i < un; i++) {
                int bn = 0;
                char **bh = db_secret_hosts(g_db, names[i], &bn);
                for (int j = 0; j < bn; j++) {
                    int dup = 0;
                    for (size_t d = 0; d < uc; d++)
                        if (strcasecmp(u[d], bh[j]) == 0) { dup = 1; break; }
                    if (dup) { free(bh[j]); continue; }
                    if (uc == ucap) {
                        size_t nc = ucap ? ucap * 2 : 4;
                        char **tmp = realloc(u, nc * sizeof(char *));
                        if (!tmp) { free(bh[j]); continue; } /* drop: narrower, fail-closed */
                        u = tmp; ucap = nc;
                    }
                    u[uc++] = bh[j];
                }
                free(bh);
            }
            secret_names_free(names, un);
            ce->bound = u; ce->bound_n = uc;
            ce->narrowed = 1;
            hosts = (const char **)u;   /* uc==0 → NULL/0 → proxy deny-all */
            hosts_n = uc;
        }
    }

    if (sens_exception && sens_exception[0]) {
        const char **ext = malloc((hosts_n + 1) * sizeof(*ext));
        if (ext) {
            for (size_t i = 0; i < hosts_n; i++) ext[i] = hosts[i];
            ext[hosts_n] = sens_exception;
            ce->ext = ext; ce->ext_n = hosts_n + 1;
            hosts = ext; hosts_n += 1;
        }
    }
    ce->hosts = hosts;
    ce->hosts_n = hosts_n;
}

static void call_egress_free(CallEgress *ce) {
    for (size_t i = 0; i < ce->deny_n; i++) free(ce->deny[i]);
    free(ce->deny);
    for (size_t i = 0; i < ce->bound_n; i++) free(ce->bound[i]);
    free(ce->bound);
    for (size_t i = 0; i < ce->decl_n; i++) free(ce->decl[i]);
    free(ce->decl);
    free((void *)ce->ext);
    memset(ce, 0, sizeof(*ce));
}

/* §4+§8 dispatch gate, split from dispatch_tool_inner (2026-07-19).
 * Capability ceiling → policy → gating hook → sensitivity/secret-bind scan →
 * approval gate, in fixed restrict-only order. Returns 0 = proceed (an
 * approved once-only sensitivity/bind park is reported via *sens_once /
 * *bind_once and sens_host for the per-call egress exception), 1 = handled
 * (error result written), 2 = parked awaiting approval. */
static int dispatch_gate(int64_t session_id, const char *agent_name,
                         PendingToolCall *tc, ToolEntry *te,
                         const ShellSecret *secrets, size_t secret_count,
                         char *sens_host, size_t sens_host_cap,
                         int *sens_once, int *bind_once) {
    int sens_hit = 0, bind_hit = 0;
    *sens_once = 0; *bind_once = 0;
    sens_host[0] = '\0';
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
        /* Session scope: a spawn-frozen tool_filter narrows the grant set for
         * this session only (grants ∩ filter). Checked after grants so the
         * filter can never widen authority, only shrink it. */
        if (!session_tool_allowed(g_db, session_id, tc->name)) {
            char err[160];
            snprintf(err, sizeof(err),
                     "error: %s blocked by this session's tool filter", tc->name);
            return tool_inline_error(session_id, tc, err, "tool_filter");
        }
        /* Malformed arguments fail closed at the gate: the model gets an
         * error tool-result and can retry. Nothing downstream (policy, hooks,
         * handlers, the child wire) ever sees invalid JSON, so no code path
         * can substitute an empty param set for unparseable args. */
        if (!tool_args_valid_object(g_db, tc->arguments)) {
            char err[160];
            snprintf(err, sizeof(err),
                     "error: %s: arguments are not a valid JSON object — "
                     "retry the call with well-formed JSON", tc->name);
            return tool_inline_error(session_id, tc, err, "bad_args");
        }
        ToolApprovalMode mode = agent_tool_mode(g_db, agent_name, tc->name);
        HookGate gate = (mode == TOOL_MODE_SILENT) ? HOOK_GATE_ALLOW : HOOK_GATE_ASK;

        /* Per-argument policy pre-filter (restrict-only, before hooks) */
        if (te->policy_json) {
            PolicyEffect pe = policy_eval(g_db, tc->arguments, te->policy_json);
            if (pe == POLICY_ERROR) {
                /* Unparseable policy (args were gate-validated above): the
                 * call is blocked — never allowed past a policy we can't
                 * read. Model-visible error; operator sees the warn. */
                LOG_WARN_("policy unparseable tool=%s — call blocked", tc->name);
                char err[160];
                snprintf(err, sizeof(err),
                         "error: %s blocked — its policy is unparseable; "
                         "report this to the operator", tc->name);
                return tool_inline_error(session_id, tc, err, "policy:error");
            }
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
        /* Sensitivity scan (restrict-only, after hooks): any labeled host in
         * the raw args forces ASK. The label list is read fresh per call —
         * SQLite is the single home for it, no cached copy to drift. */
        {
            int sn = 0;
            char **sens = db_sensitive_hosts(g_db, &sn);
            if (sens) {
                sens_hit = host_in_text(sens, (size_t)sn, tc->arguments,
                                        sens_host, sens_host_cap);
                /* Second chance on the *extracted, decoded* url host: a
                 * percent- or invisible-Unicode-encoded host tokenizes wrong
                 * in the raw-args scan above, which would skip the ASK
                 * escalation (r2 F11). */
                if (!sens_hit) {
                    char uh[254];
                    if (web_args_url_host(tc->arguments, uh, sizeof(uh))) {
                        url_host_normalize(uh, sizeof(uh));
                        if (host_covered(sens, (size_t)sn, uh)) {
                            sens_hit = 1;
                            snprintf(sens_host, sens_host_cap, "%s", uh);
                        }
                    }
                }
                for (int i = 0; i < sn; i++) free(sens[i]);
                free(sens);
            }
            if (sens_hit && gate < HOOK_GATE_ASK) gate = HOOK_GATE_ASK;
        }
        /* Fail-closed credential rule (specs/trust.md): submitting a loaded
         * secret is gated by its host bindings. First use (no bindings) parks;
         * a web call whose url host isn't covered by every used secret's
         * bindings parks. shell/js have no static target — their enforcement
         * is the egress narrowing in call_egress_build below. */
        {
            size_t un = 0;
            char **names = used_secret_names(tc->arguments, secrets, secret_count, &un);
            if (names) {
                char url_host[254] = "";
                int have_url = (te->recipe.tier == SBX_WEB) &&
                    web_args_url_host(tc->arguments, url_host, sizeof(url_host));
                for (size_t i = 0; i < un && !bind_hit; i++) {
                    int bn = 0;
                    char **bh = db_secret_hosts(g_db, names[i], &bn);
                    if (!bh) {
                        bind_hit = 1;   /* first use: no bindings yet */
                    } else {
                        if (have_url && !host_covered(bh, (size_t)bn, url_host))
                            bind_hit = 1;
                        for (int j = 0; j < bn; j++) free(bh[j]);
                        free(bh);
                    }
                }
                secret_names_free(names, un);
            }
            if (bind_hit && gate < HOOK_GATE_ASK) gate = HOOK_GATE_ASK;
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
                /* none → create + park; pending → re-park idempotently.
                 * A sensitivity hit is recorded as action='sensitive' so the
                 * resolve path can refuse to mint anything standing from it. */
                if (!pending)
                    approval_create(g_db, session_id, tc->call_id, tc->name,
                                    sens_hit ? "sensitive"
                                             : (bind_hit ? "secret_bind" : tc->name),
                                    tc->arguments, "rerun");
                session_set_state(g_db, session_id, "awaiting_approval");
                approval_free(ap);
                handle_approval_park(session_id);
                return 2; /* parked */
            }
            /* approved → consume (single-use) and fall through to execute the
             * frozen call. An ALWAYS decision flips the tool mode to silent so
             * it never reaches the gate again; only a "once" approval lands
             * here, and consuming it stops a replayed tool_call_id from
             * re-using the same grant. A consumed sensitivity approval opens a
             * per-call exception for the matched host (network tiers below). */
            *sens_once = sens_hit;
            *bind_once = bind_hit;
            approval_consume(g_db, ap->id);
            approval_free(ap);
        }
    }
    return 0;
}

/* EXEC_INLINE vehicle: run the handler on the event-loop thread, split
 * from dispatch_tool_inner (2026-07-19). Return codes match dispatch:
 * 0 wait, 1 handled, 2 parked, 3 parallel-safe async. */
static int dispatch_inline(int64_t session_id, const char *agent_name,
                           PendingToolCall *tc, ToolEntry *te,
                           const ShellSecret *secrets, size_t secret_count) {
    /* Interpolate {{SECRET:X}} only for tools that exec with credentials */
    char *interp_args = NULL;
    if (tool_needs_interpolation(tc->name) && secret_count > 0)
        interp_args = secret_interpolate(tc->arguments, secrets, secret_count);
    /* Thread the live session + tool_call_id into the per-tool context.
     * g_tool_setup is a single shared instance, so the session_id captured
     * at agent_setup_init time is stale (0 in CLI) — the dispatching session
     * varies per call (root or any sub-agent). Keyed on ctx *pointer
     * identity*, not on an enumerated tool-name list: launch_agent and
     * check_session share launch_ctx.
     * Any future tool registered against these same shared ctx structs
     * is covered automatically; a tool needing its own fresh ctx should
     * get its own struct, not reuse one of these without adding it here. */
    if (te->user_data == &g_tool_setup->launch_ctx) {
        AgentLaunchCtx *lc = (AgentLaunchCtx *)te->user_data;
        lc->session_id = session_id;
        lc->current_tool_call_id = tc->call_id;
    } else if (te->user_data == &g_tool_setup->req_cfg_ctx) {
        RequestConfigCtx *rctx = (RequestConfigCtx *)te->user_data;
        rctx->session_id = session_id;
        rctx->current_tool_call_id = tc->call_id;
    } else if (te->user_data == &g_tool_setup->bootstrap_ctx) {
        ToolBootstrapCtx *bctx = (ToolBootstrapCtx *)te->user_data;
        bctx->session_id = session_id;
        bctx->current_tool_call_id = tc->call_id;
    } else if (te->user_data == &g_tool_setup->ext_tool_ctx) {
        ToolExtensionCtx *ectx = (ToolExtensionCtx *)te->user_data;
        ectx->session_id = session_id;
        ectx->current_tool_call_id = tc->call_id;
    } else if (te->user_data == &g_tool_setup->chan_send_ctx) {
        ToolChannelSendCtx *cctx = (ToolChannelSendCtx *)te->user_data;
        cctx->session_id = session_id;
        /* Route allowlist must key on the advancing agent, not the
         * setup's init agent. */
        snprintf(cctx->agent_name, sizeof(cctx->agent_name), "%s",
                 agent_name ? agent_name : "");
    }
    char *result = te->handler(interp_args ? interp_args : tc->arguments, te->user_data);
    if (interp_args) { explicit_bzero(interp_args, strlen(interp_args)); free(interp_args); }
    /* A NULL result means the tool dispatched async work and left this
     * tool_call without an inline result. Shape depends on the tool's
     * null_kind trait (see tool_traits above). */
    if (!result) {
        const ToolTraits *t = tool_traits_lookup(tc->name);
        ToolNullKind nk = t ? t->null_kind : NULL_NONE;
        if (nk == NULL_ASYNC) {
            /* Sub-agent launched. The call is already 'running' (claimed at
             * dispatch), so the turn-join (advance_session, tool_running)
             * neither re-dispatches it nor proceeds to the LLM until the child
             * completes and writes the result keyed by this call_id. The
             * parent stays tool_running, so sibling launch_agent calls keep
             * dispatching → real parallelism. */
            /* parallel-safe tools let dispatch continue to siblings (3);
             * a serial tool would stop and wait (0). */
            return tool_is_parallel_safe(tc->name) ? 3 : 0;
        }
        if (nk == NULL_PARK) {
            /* Approval gate: the session is parked in awaiting_approval; the
             * dispatch loop releases this call's claim back to pending, where
             * it stays until resolve_approval re-runs it or writes the result. */
            handle_approval_park(session_id);
            return 2; /* parked, don't advance */
        }
        result = strdup("error: tool returned null");
    }

    /* Explicit capture first (needs the RAW result), then postprocess:
     * deinterpolate + secret scan/redact (scan runs even with no secrets
     * loaded — inline js_eval output can carry leaked credentials). */
    { char *cap = secret_capture_apply(g_db, tc->arguments, result);
      if (cap) { free(result); result = cap; } }
    { char *pp = tool_result_postprocess(result, secrets, secret_count);
      if (pp) { free(result); result = pp; } }

    /* afterToolCall hooks: chained result replacement, after the secret
     * scanner (security boundary stays first), before the entry write so
     * the replacement is what the context sees. The replacement comes back
     * already marker-sanitized (hook_dispatch owns that invariant). */
    char *hook_annotate = NULL;
    if (g_tool_setup) {
        char *rep = hook_dispatch_observe_tool_call(&g_tool_setup->ext_ctx,
                        g_db, tc->name, tc->arguments, result, &hook_annotate);
        if (rep) { free(result); result = rep; }
    }

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

    /* Ensure valid UTF-8 before DB storage */
    { char *clean = utf8_sanitize(result, strlen(result));
      if (clean) { free(result); result = clean; } }

    char *stored = truncate_and_spill(result, session_id, tc->call_id);
    ToolResult tr = {.tool_call_id = tc->call_id,
                     .content = stored ? stored : result};
    int is_err = (strncmp(result, "error:", 6) == 0);
    Message msg = {.role = ROLE_TOOL, .tool_result = &tr,
                   .tool_name = tc->name, .is_error = is_err};
    int64_t rid = entry_append_with_turn(g_db, session_id, &msg, tc->turn_id);
    db_tool_call_complete_with_result(g_db, tc->entry_id, tc->call_id, rid);
    if (hook_annotate) {
        if (rid > 0) hook_entry_data_patch(g_db, rid, hook_annotate);
        free(hook_annotate);
    }
    free(stored);
    free(result);
    LOG_INFO_("tool done tool=%s inline=1", tc->name);
    return 1; /* Handled inline */

}

/* Resolve+create the agent's scratch dir, bound as /tmp in the child. Done in
 * the parent so the "is this a 0700 dir we own?" check happens once in the
 * trusted process rather than in each sandboxed child. Returns NULL (and the
 * child then gets only the tiny root tmpfs as /tmp) rather than falling back to
 * anything wider — notably never the host's shared /tmp. */
static const char *dispatch_scratch_dir(const char *agent_name,
                                        char *out, size_t cap) {
    char *root = g_db ? config_get(g_db, "tmp_root") : NULL;
    const char *r = scratch_dir_ensure(agent_name, root, out, cap);
    free(root);
    return r;
}

static int dispatch_tool_inner(int64_t session_id, const char *agent_name,
                               PendingToolCall *tc,
                               const ShellSecret *secrets, size_t secret_count) {
    if (g_child_count >= CHILD_MAX) return -1;

    ToolEntry *te = g_tool_setup ? tools_lookup(&g_tool_setup->reg, tc->name) : NULL;
    if (!te && g_tool_setup) {
        /* Registry is a cache of the extension-tool query; a miss may just
         * mean an extension was promoted/attached after startup, or that this
         * is a non-default agent whose tools were never materialized. Refresh
         * for the *advancing* agent and retry. Safe unlocked: the registry is
         * only touched on the event-loop thread. On TOOLS_MAX overflow the
         * register fails, the second lookup misses, and we fall through to the
         * unknown-tool error instead of corrupting the registry. */
        tools_load_extension_tools(&g_tool_setup->reg, g_db, agent_name,
                                   &g_tool_setup->js_eval_ctx);
        te = tools_lookup(&g_tool_setup->reg, tc->name);
        if (!te)
            LOG_WARN_("tool '%s' not registered after extension reload (agent=%s)",
                      tc->name, agent_name);
    }
    if (!te) {
        /* Unknown tool — write error result directly */
        char err[192];
        snprintf(err, sizeof(err),
                 "error: unknown tool '%s' — use search_config to see available tools",
                 tc->name);
        ToolResult tr = {.tool_call_id = tc->call_id, .content = err};
        Message msg = {.role = ROLE_TOOL, .tool_result = &tr,
                       .tool_name = tc->name, .is_error = 1};
        entry_append_with_turn(g_db, session_id, &msg, tc->turn_id);
        db_tool_call_set_status(g_db, session_id, tc->call_id, "done", NULL);
        return 1; /* Signal: handled inline, check for more */
    }

    /* Dispatch gate (capability → policy → hooks → sensitivity/bind →
     * approval). sens_host survives the gate: an approved once-only
     * sensitivity park grants that host to THIS call via a per-call
     * exception in the network-tier sections below. */
    char sens_host[254];
    int sens_once = 0, bind_once = 0;
    {
        int g = dispatch_gate(session_id, agent_name, tc, te,
                              secrets, secret_count,
                              sens_host, sizeof(sens_host),
                              &sens_once, &bind_once);
        if (g != 0) return g;
    }

    if (te->recipe.vehicle == EXEC_INLINE)
        return dispatch_inline(session_id, agent_name, tc, te,
                               secrets, secret_count);


    /* ── File-tier re-exec path: fork+exec a clean --run-tool child ────
     * Replaces the fork-only path for file tools when sandbox is required.
     * The daemon is multithreaded; fork-without-exec is UB (frozen locks in
     * child). The re-exec'd child is single-threaded and sets up its own
     * namespace sandbox. */
    if (te->recipe.vehicle == EXEC_SANDBOX && te->recipe.tier == SBX_FILE &&
        te->user_data) {
        FileReadCtx *fctx = (FileReadCtx *)te->user_data;
        if (fctx->workspace) {
            /* CLI progress */
            if (g_mode == 0)
                cli_print_tool_call(tc->name, tc->arguments);
            session_set_state(g_db, session_id, "tool_running");

            /* Host mode (sandbox=0) rides the SAME broker path: the child sets
             * up no namespace and run_file_tier runs the handler in-process,
             * exactly as the (now-deleted) generic fork path did. */
            /* Decompose arguments HERE (trusted parent, SQLite JSON1) — the
             * child receives pre-extracted params and parses no JSON. */
            ToolWireArg *params = NULL;
            size_t param_n = 0;
            if (tool_args_extract(g_db, tc->name, te->parameters_json,
                                  tc->arguments, &params, &param_n) != 0)
                return tool_inline_error(session_id, tc,
                    "error: invalid tool arguments", "bad_args");

            size_t blob_len = 0;
            RunToolReq req;
            run_tool_req_init(&req, RUNTOOL_TIER_FILE, tc->name,
                              &fctx->sb, fctx->workspace, fctx->cwd_path,
                              fctx->db_path);
            char scratch[PATH_MAX];
            req.tmp_dir = dispatch_scratch_dir(agent_name, scratch, sizeof(scratch));
            req.params = params;
            req.param_count = param_n;

            /* Mount skill discovery dirs read-only (same transient-override
             * pattern as the extension store on the JS path) so the model can
             * file_read a SKILL.md the prompt index pointed it at. The shared
             * extension store rides along for the same reason: attached
             * extensions' declared skills live there. */
            size_t skd_n = 0;
            char agents_dir_buf[PATH_MAX];
            agent_dir_resolve(fctx->workspace, g_cfg ? g_cfg->db_path : NULL,
                              agents_dir_buf, sizeof(agents_dir_buf));
            char **skill_dirs = skills_dirs_resolve(g_db, agents_dir_buf,
                                                    agent_name, &skd_n);
            char store_dir[PATH_MAX] = {0};
            int have_store = 0;
            if (g_cfg && g_cfg->db_path && g_cfg->db_path[0]) {
                char tmp[PATH_MAX - 16];
                snprintf(tmp, sizeof(tmp), "%s", g_cfg->db_path);
                char *sl = strrchr(tmp, '/');
                if (sl) {
                    *sl = '\0';
                    snprintf(store_dir, sizeof(store_dir), "%s/extensions", tmp);
                    struct stat stsb;
                    if (stat(store_dir, &stsb) == 0 && S_ISDIR(stsb.st_mode))
                        have_store = 1;
                }
            }
            const char **read_paths = NULL;
            size_t rp_count = fctx->sb.read_path_count + skd_n +
                              (have_store ? 1 : 0);
            if (rp_count > 0) {
                read_paths = malloc(rp_count * sizeof(*read_paths));
                if (read_paths) {
                    size_t k = 0;
                    for (size_t i = 0; i < fctx->sb.read_path_count; i++)
                        read_paths[k++] = fctx->sb.read_paths[i];
                    for (size_t i = 0; i < skd_n; i++)
                        read_paths[k++] = skill_dirs[i];
                    if (have_store) read_paths[k++] = store_dir;
                    req.read_paths = read_paths;
                    req.read_count = rp_count;
                }
            }

            char *blob = run_tool_serialize_request(&req, &blob_len);
            free(read_paths);
            skills_dirs_free(skill_dirs, skd_n);
            tool_wire_args_free(params, param_n);
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
            LOG_INFO_("tool fork tool=%s", tc->name);
            return 0; /* async serial — wait for reap */
        }
    }

    /* ── Shell-tier re-exec path: fork+exec a clean --run-tool broker ────
     * A broker is interposed IFF the tier needs gated egress (web, shell).
     * The broker IS the --run-tool process — fork+exec, never fork-only.
     * Network-less tiers (file) spawn directly via spawn_run_tool_child.
     * Secrets: interpolated HERE in the daemon parent, blob carries resolved
     * values. Broker/child never hold the master key. */
    if (te->recipe.vehicle == EXEC_SANDBOX && te->recipe.tier == SBX_SHELL &&
        te->user_data) {
        ShellConfig *sc = (ShellConfig *)te->user_data;
        if (sc->workspace) {
            /* Host mode (sandbox=0) rides the SAME broker path: the child execs
             * /bin/sh with no namespace (sandbox is threaded into SandboxConfig),
             * replacing the deleted host-mode fork-only branch. */
            if (g_mode == 0)
                cli_print_tool_call(tc->name, tc->arguments);
            session_set_state(g_db, session_id, "tool_running");

            char *command = tool_args_str(g_db, tc->arguments, "command");
            if (!command)
                return tool_inline_error(session_id, tc,
                    "error: shell_exec requires a 'command' string argument", NULL);
            int cmd_timeout = tool_args_int(g_db, tc->arguments, "timeout", sc->timeout);
            if (cmd_timeout <= 0) cmd_timeout = sc->timeout;

            /* Parent-side secret interpolation (daemon holds the key) */
            char *interp_cmd = NULL;
            if (secret_count > 0)
                interp_cmd = secret_interpolate(command, secrets, secret_count);
            const char *resolved_cmd = interp_cmd ? interp_cmd : command;

            /* Filter secrets to minimal set (only those referenced by command) */
            size_t min_count = 0;
            RunToolSecret *min_secrets = shell_filter_secrets(
                resolved_cmd, sc->secrets, sc->secret_count, &min_count);

            /* Resolve agent_dir for proxy socket */
            char agent_dir[PATH_MAX];
            agent_dir_resolve(sc->workspace, sc->db_path, agent_dir, sizeof(agent_dir));

            size_t blob_len = 0;
            RunToolReq req;
            run_tool_req_init(&req, RUNTOOL_TIER_SHELL, tc->name,
                              &sc->sb, sc->workspace, sc->cwd_path,
                              sc->db_path);
            char scratch[PATH_MAX];
            req.tmp_dir = dispatch_scratch_dir(agent_name, scratch, sizeof(scratch));
            req.agent_dir = agent_dir;
            CallEgress se;
            call_egress_build(&se, tc->name, tc->arguments, bind_once,
                              sens_once ? sens_host : NULL,
                              sc->allowed_hosts, sc->allowed_host_count,
                              secrets, secret_count);
            req.host_rules = se.hosts;
            req.host_count = se.hosts_n;
            req.deny_rules = (const char **)se.deny;
            req.deny_count = se.deny_n;
            req.command = resolved_cmd;
            req.shell_path = sc->shell_path;
            req.timeout = cmd_timeout;
            req.secrets = min_secrets;
            req.secret_count = min_count;
            char *blob = run_tool_serialize_request(&req, &blob_len);
            call_egress_free(&se);

            /* Wipe interpolated command (contains secret plaintext) */
            if (interp_cmd) { explicit_bzero(interp_cmd, strlen(interp_cmd)); free(interp_cmd); }
            free(min_secrets);
            free(command);

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
            LOG_INFO_("tool fork tool=%s", tc->name);
            return 0;
        }
    }

    /* ── Web-tier broker path (SBX_WEB): the same fork+execve --run-tool broker
     * as shell, but the inner child runs OUR curl in-process (no inner exec).
     * Egress is decided per-hop by the proxy — no pre-flight. Host mode rides the
     * same path with sandbox=0 (no namespace, no proxy). */
    if (te->recipe.vehicle == EXEC_SANDBOX && te->recipe.tier == SBX_WEB &&
        te->user_data) {
        WebFetchCtx *wc = (WebFetchCtx *)te->user_data;
        if (g_mode == 0)
            cli_print_tool_call(tc->name, tc->arguments);
        session_set_state(g_db, session_id, "tool_running");

        /* Decompose arguments in the parent, then interpolate {{SECRET:X}}
         * per extracted value (a URL may carry a token). */
        ToolWireArg *params = NULL;
        size_t param_n = 0;
        if (tool_args_extract(g_db, tc->name, te->parameters_json,
                              tc->arguments, &params, &param_n) != 0)
            return tool_inline_error(session_id, tc,
                "error: invalid tool arguments", "bad_args");
        wire_params_interpolate(params, param_n, secrets, secret_count);

        char agent_dir[PATH_MAX];
        agent_dir_resolve(wc->workspace, wc->db_path, agent_dir, sizeof(agent_dir));

        size_t blob_len = 0;
        RunToolReq req;
        run_tool_req_init(&req, RUNTOOL_TIER_WEB, tc->name,
                          &wc->sb, wc->workspace, wc->cwd_path,
                          wc->db_path);
        char scratch[PATH_MAX];
        req.tmp_dir = dispatch_scratch_dir(agent_name, scratch, sizeof(scratch));
        req.params = params;
        req.param_count = param_n;
        req.agent_dir = agent_dir;
        CallEgress se;
        call_egress_build(&se, tc->name, tc->arguments, bind_once,
                          sens_once ? sens_host : NULL,
                          wc->allowed_hosts, wc->allowed_host_count,
                          secrets, secret_count);
        req.host_rules = se.hosts;
        req.host_count = se.hosts_n;
        req.deny_rules = (const char **)se.deny;
        req.deny_count = se.deny_n;
        req.timeout = 60;
        char *blob = run_tool_serialize_request(&req, &blob_len);
        call_egress_free(&se);
        tool_wire_args_wipe_free(params, param_n);
        if (!blob)
            return tool_inline_error(session_id, tc,
                "error: web request exceeds 32KB cap", NULL);
        int rc = spawn_run_tool_child(session_id, agent_name,
                     tc->call_id, tc->name, tc->arguments,
                     tc->turn_id, tc->entry_id, blob, blob_len, 90);
        explicit_bzero(blob, blob_len);
        free(blob);
        if (rc != 0)
            return tool_inline_error(session_id, tc,
                "error: spawn_run_tool_child failed", "fork_failed");
        LOG_INFO_("tool fork tool=%s", tc->name);
        return 0;
    }

    /* ── JS-tier broker path (SBX_JS): web's twin. The inner child evals qjs
     * in-process (no inner exec); http_request's curl reaches the proxy via
     * HTTP_PROXY → decide() per hop. fs.* paths are real bind-mounts; the shared
     * extension store is mounted read-only so promoted handler files resolve.
     * Covers both js_eval and JS-defined extension tools (resolved via the same
     * entry's recipe). Host mode rides the same path with sandbox=0. */
    if (te->recipe.vehicle == EXEC_SANDBOX && te->recipe.tier == SBX_JS &&
        te->user_data) {
        JsEvalCtx *jc = NULL;
        const char *handler_path = NULL;
        if (js_tool_resolve_request(te, &jc, &handler_path) != 0 || !jc)
            return tool_inline_error(session_id, tc,
                "error: js tool request resolution failed", NULL);

        if (g_mode == 0)
            cli_print_tool_call(tc->name, tc->arguments);
        session_set_state(g_db, session_id, "tool_running");

        /* Params: js_eval ships its own schema-extracted code/filename/args;
         * an extension tool ships filename=<handler .qjs path> plus the raw
         * call arguments as the opaque `args` blob QuickJS parses natively —
         * no JSON wrapping/escaping round-trip. Then interpolate {{SECRET:X}}
         * per value (args may carry a token for http_request). */
        ToolWireArg *params = NULL;
        size_t param_n = 0;
        if (!handler_path) {
            if (tool_args_extract(g_db, tc->name, te->parameters_json,
                                  tc->arguments, &params, &param_n) != 0)
                return tool_inline_error(session_id, tc,
                    "error: invalid tool arguments", "bad_args");
        } else {
            params = calloc(2, sizeof(*params));
            if (params) {
                params[0].key = strdup("filename");
                params[0].kind = TOOL_ARG_TEXT;
                params[0].value = strdup(handler_path);
                params[1].key = strdup("args");
                params[1].kind = TOOL_ARG_JSON;
                params[1].value = strdup(tc->arguments && tc->arguments[0]
                                             ? tc->arguments : "{}");
                param_n = 2;
            }
            if (!params || !params[0].key || !params[0].value ||
                !params[1].key || !params[1].value) {
                tool_wire_args_free(params, param_n);
                return tool_inline_error(session_id, tc,
                    "error: OOM building js request", NULL);
            }
        }
        wire_params_interpolate(params, param_n, secrets, secret_count);

        char agent_dir[PATH_MAX];
        agent_dir_resolve(jc->workspace, jc->db_path, agent_dir, sizeof(agent_dir));

        /* Mount the shared extension store read-only (<db_dir>/extensions), in
         * addition to the agent's own read grants, so promoted handler files
         * load without an explicit grant. Build a transient read-path array. */
        char store_dir[PATH_MAX] = {0};
        int have_store = 0;
        if (jc->db_path && jc->db_path[0]) {
            char tmp[PATH_MAX - 16];
            snprintf(tmp, sizeof(tmp), "%s", jc->db_path);
            char *sl = strrchr(tmp, '/');
            if (sl) {
                *sl = '\0';
                snprintf(store_dir, sizeof(store_dir), "%s/extensions", tmp);
                struct stat sb;
                if (stat(store_dir, &sb) == 0 && S_ISDIR(sb.st_mode)) have_store = 1;
            }
        }
        size_t rc_count = jc->sb.read_path_count + (have_store ? 1 : 0);
        const char **read_paths = NULL;
        if (rc_count > 0) {
            read_paths = malloc(rc_count * sizeof(*read_paths));
            if (read_paths) {
                size_t k = 0;
                for (size_t i = 0; i < jc->sb.read_path_count; i++)
                    read_paths[k++] = jc->sb.read_paths[i];
                if (have_store) read_paths[k++] = store_dir;
            } else {
                rc_count = 0;
            }
        }

        size_t blob_len = 0;
        RunToolReq req;
        run_tool_req_init(&req, RUNTOOL_TIER_JS, "js_eval",
                          &jc->sb, jc->workspace, jc->cwd_path,
                          jc->db_path);
        char scratch[PATH_MAX];
        req.tmp_dir = dispatch_scratch_dir(agent_name, scratch, sizeof(scratch));
        req.params = params;
        req.param_count = param_n;
        req.read_paths = read_paths;  /* transient override: + extension store */
        req.read_count = rc_count;
        req.agent_dir = agent_dir;
        CallEgress se;
        call_egress_build(&se, tc->name, tc->arguments, bind_once,
                          sens_once ? sens_host : NULL,
                          jc->allowed_hosts, jc->allowed_hosts_count,
                          secrets, secret_count);
        req.host_rules = se.hosts;
        req.host_count = se.hosts_n;
        req.deny_rules = (const char **)se.deny;
        req.deny_count = se.deny_n;
        req.timeout = 120;
        char *blob = run_tool_serialize_request(&req, &blob_len);
        call_egress_free(&se);
        free(read_paths);
        tool_wire_args_wipe_free(params, param_n);
        if (!blob)
            return tool_inline_error(session_id, tc,
                "error: js request exceeds 32KB cap", NULL);
        int rc = spawn_run_tool_child(session_id, agent_name,
                     tc->call_id, tc->name, tc->arguments,
                     tc->turn_id, tc->entry_id, blob, blob_len, 150);
        explicit_bzero(blob, blob_len);
        free(blob);
        if (rc != 0)
            return tool_inline_error(session_id, tc,
                "error: spawn_run_tool_child failed", "fork_failed");
        LOG_INFO_("tool fork tool=%s", tc->name);
        return 0;
    }

    /* ── EXEC_THREAD: fire-and-forget detached thread for DB/session-only
     * tools. The tool runs to completion on its own thread with its OWN sqlite3*
     * (never the registry handler's captured handle, which is the poll thread's),
     * writes the result + completes the call, then wakes the poll loop via the
     * tool-notify pipe → run_advance. The poll loop never blocks on tool logic. */
    if (te->recipe.vehicle == EXEC_THREAD && te->recipe.thread_run) {
        if (g_mode == 0)
            cli_print_tool_call(tc->name, tc->arguments);
        session_set_state(g_db, session_id, "tool_running");

        ToolThreadJob job = {0};
        job.session_id = session_id;
        job.turn_id = tc->turn_id;
        job.entry_id = tc->entry_id;
        snprintf(job.tool_call_id, sizeof(job.tool_call_id), "%s", tc->call_id);
        snprintf(job.tool_name, sizeof(job.tool_name), "%s", tc->name);
        snprintf(job.agent_name, sizeof(job.agent_name), "%s", agent_name);
        job.args = strdup(tc->arguments ? tc->arguments : "{}");
        job.run = te->recipe.thread_run;
        /* No THREAD (DB) tool references {{SECRET}} — interpolation is a
         * sandbox-tier concern. The thread still postprocesses (DLP scan). */
        job.secrets = g_tool_setup ? g_tool_setup->secrets : NULL;
        job.secret_count = g_tool_setup ? g_tool_setup->secret_count : 0;

        /* Already 'running' (claimed at dispatch) so a re-advance landing
         * while the thread runs can't double-dispatch; the thread flips it
         * to done. */
        if (tool_thread_spawn(&job) != 0)
            return tool_inline_error(session_id, tc,
                "error: failed to spawn tool thread", "thread_failed");
        return 0; /* serial async — wait for the thread's completion notify */
    }

    /* No recipe matched — unreachable in practice. Fail the call so the turn
     * advances instead of hanging on a pending tool_call. */
    return tool_inline_error(session_id, tc,
        "error: tool has no execution recipe", NULL);
}

/* Per-call secret snapshot wrapper: env base (g_tool_setup->secrets, immutable
 * for the process lifetime) merged with a fresh read of the DB-backed secret
 * store, so a secret born mid-session (operator `secret set`, secret_create)
 * is visible on its very next dispatch. Scoped to this one call — never
 * shared with async work that outlives it (EXEC_THREAD re-snapshots itself
 * with its own db handle; see tool_thread.c). */
static int dispatch_tool(int64_t session_id, const char *agent_name,
                         PendingToolCall *tc) {
    size_t snap_n = 0;
    ShellSecret *snap = secrets_snapshot(g_db,
        g_tool_setup ? g_tool_setup->secrets : NULL,
        g_tool_setup ? g_tool_setup->secret_count : 0, &snap_n);
    int rc = dispatch_tool_inner(session_id, agent_name, tc, snap, snap_n);
    secrets_snapshot_free(snap, snap_n);
    return rc;
}

/* ── Pipe draining helpers ─────────────────────────────────────── */

/* Largest hosts-JSON meta accepted from a child; larger metas are drained and
 * discarded (hosts_json stays NULL) so a misbehaving child can't balloon us. */
#define FRAME_META_MAX (64 * 1024)

/* Drain a tool child's result pipe (nonblocking): parse the frame header +
 * hosts meta, then accumulate the result body into c->outbuf, kept
 * NUL-terminated. Body bytes beyond TOOL_MAX_OUTPUT are read and discarded so
 * the child never blocks on a full pipe. Closes the fd on EOF or error;
 * leaves it open on EAGAIN (more data may come). */
static void child_drain_pipe(ChildProc *c) {
    if (c->result_pipe < 0) return;

    char buf[4096];
    ssize_t n;
    while ((n = read(c->result_pipe, buf, sizeof(buf))) > 0) {
        size_t off = 0;

        /* Frame header: 4-byte network-order meta length, may arrive split. */
        while (c->frame_hdr_read < 4 && off < (size_t)n)
            c->frame_hdr[c->frame_hdr_read++] = (unsigned char)buf[off++];
        if (c->frame_hdr_read == 4 && c->frame_meta_read == 0 && !c->hosts_json) {
            c->frame_meta_len = ((size_t)c->frame_hdr[0] << 24) |
                                ((size_t)c->frame_hdr[1] << 16) |
                                ((size_t)c->frame_hdr[2] << 8)  |
                                 (size_t)c->frame_hdr[3];
            if (c->frame_meta_len > 0 && c->frame_meta_len <= FRAME_META_MAX &&
                !c->hosts_json)
                c->hosts_json = calloc(1, c->frame_meta_len + 1);
        }
        if (c->frame_hdr_read < 4) continue;

        /* Meta bytes (hosts JSON); oversized meta is consumed but dropped. */
        while (c->frame_meta_read < c->frame_meta_len && off < (size_t)n) {
            size_t want = c->frame_meta_len - c->frame_meta_read;
            size_t avail = (size_t)n - off;
            size_t take = want < avail ? want : avail;
            if (c->hosts_json)
                memcpy(c->hosts_json + c->frame_meta_read, buf + off, take);
            c->frame_meta_read += take;
            off += take;
        }
        if (off >= (size_t)n) continue;

        /* Result body. */
        size_t to_copy = (size_t)n - off;
        if (c->outbuf_len + to_copy > TOOL_MAX_OUTPUT)
            to_copy = TOOL_MAX_OUTPUT - c->outbuf_len;
        if (to_copy == 0) continue; /* at cap: keep draining, discard */
        char *tmp = realloc(c->outbuf, c->outbuf_len + to_copy + 1);
        if (!tmp) continue; /* OOM: drop chunk, keep child unblocked */
        memcpy(tmp + c->outbuf_len, buf + off, to_copy);
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

/* The --run-tool child gets a minimal env: just the log level, so its
 * syslog output (proxy denials, sandbox failures) honors the configured
 * verbosity. Prebuilt at startup — the fork child is async-signal-safe. */
static char g_log_level_env[32] = "CCLAW_LOG_LEVEL=info";

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
        char *const envp[] = {g_log_level_env, NULL};
        execve("/proc/self/exe", argv, envp);
        /* execve failed — write a framed static error (4-byte zero meta_len
         * header, then the message) to fd 3 and die */
        (void)write(FD_REQUEST, "\0\0\0\0error: execve failed", 24);
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
    util_set_nonblock(sp[0]);

    /* Register child — mirrors dispatch_tool bookkeeping */
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

#define POLL_DB_INTERVAL 30  /* seconds between DB polls (heartbeat, recovery, approvals) */
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
        resolve_approval(ids[i], APPROVAL_DENY, "auto:expired", 0);
    free(ids);
}

/* approval_timeout_sec — the same deadline approval.c uses to park-expire;
 * reused by --auto-approve to bound its self-expiring grants. */
static int approval_timeout_seconds(void) {
    int timeout = config_get_int(g_db, "approval_timeout_sec");
    return timeout > 0 ? timeout : config_default_int("approval_timeout_sec");
}

/* approval_block_sec, clamped to approval_timeout_sec so the short block
 * never outlasts the final expiry deadline. */
static int approval_block_seconds(void) {
    int block = config_get_int(g_db, "approval_block_sec");
    if (block <= 0) block = config_default_int("approval_block_sec");
    int timeout = approval_timeout_seconds();
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
    /* Besides queued inbox rows, also pick up sessions whose leaf entry is an
     * unanswered user entry: a refused dispatch (rate limit, disk floor, full
     * pool) consumed the inbox then parked the session idle/rate_limited, so
     * no inbox row remains to trigger a retry. Advancing them re-runs the
     * dispatch gates once pressure clears (rate_limited flips to idle on the
     * first advance, dispatches on the next tick). */
    const char *sql =
        "SELECT s.id FROM sessions s WHERE s.state IN ('idle','rate_limited')"
        "  AND (EXISTS (SELECT 1 FROM inbox i"
        "               WHERE i.session_id=s.id AND i.consumed=0)"
        "    OR EXISTS (SELECT 1 FROM entries e"
        "               WHERE e.id=s.leaf_id AND e.session_id=s.id AND e.role=1))"
        " LIMIT 64;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return;
    int cap = 0, n = 0;
    int64_t *ids = NULL;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n >= cap) {
            cap = cap ? cap * 2 : 16;
            int64_t *tmp = realloc(ids, (size_t)cap * sizeof(*ids));
            if (!tmp) { free(ids); sqlite3_finalize(st); return; }
            ids = tmp;
        }
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
    db_prune_inbox(g_db);
    db_prune_outbox(g_db);
    db_wal_checkpoint(g_db);   /* truncate WAL — passive checkpoint can stall on a long reader */

    /* Disk floor monitoring: log loudly on crossing the threshold (edge-
     * triggered so it doesn't spam every poll). The dispatch_llm gate does the
     * actual refusing; this makes the low-disk state visible in the journal. */
    static int disk_low = 0;
    int disk_floor_mb = config_default_int("disk_min_free_mb");
    long free_mb = db_free_mb(g_db);
    if (free_mb >= 0 && disk_floor_mb > 0) {
        if (free_mb < disk_floor_mb && !disk_low) {
            LOG_WARN_("disk_low free_mb=%ld floor_mb=%d refusing new llm dispatch",
                      free_mb, disk_floor_mb);
            disk_low = 1;
        } else if (free_mb >= disk_floor_mb && disk_low) {
            LOG_INFO_("disk_recovered free_mb=%ld floor_mb=%d", free_mb, disk_floor_mb);
            disk_low = 0;
        }
    }

    if (g_mode == 1) {
        session_sweep_inbox();
        cron_run_due(g_db);
    }
}

/* ── apply_grant: apply an 'apply'-style capability grant ──────────
 * Extracted from resolve_approval so both the in-window path and the
 * post-window inbox path apply grants identically. Called only for an approved
 * (APPROVAL_ALWAYS) apply approval. Sets *rename_failed if a rename's disk step
 * failed (DB change rolled back); refreshes live caps on success.
 * grant_expires_at: 0 for a permanent grant, else a future unix timestamp
 * (--auto-approve path — see resolve.h). */
static void apply_grant(const Approval *a, const char *agent, int *rename_failed,
                        int64_t grant_expires_at) {
    *rename_failed = 0;
    if (strcmp(a->action, "request_changes") == 0) {
        /* One savepoint applies the whole document — grants, config values,
         * provider upsert (tool_request_config.c). Failure rolls everything
         * back and surfaces as apply-denied, never a partial grant set. */
        if (request_config_changes_apply(g_db, agent, a->args_json,
                                         grant_expires_at) != 0) {
            LOG_WARN_("request_changes apply failed");
            *rename_failed = 1; /* generic apply-failed: error result, no grant */
        }
    } else if (strcmp(a->action, "extension_promote") == 0) {
        char *bundle = tool_args_str(g_db, a->args_json, "bundle");
        char *ierr = NULL;
        if (!bundle || extension_install(g_db, bundle, agent, &ierr) != 0) {
            LOG_WARN_("extension_promote apply failed: %s", ierr ? ierr : "no bundle");
            *rename_failed = 1;
        }
        free(ierr);
        free(bundle);
    } else if (strcmp(a->action, "create_agent") == 0) {
        char *cerr = NULL;
        const char *adir = g_tool_setup ? g_tool_setup->req_cfg_ctx.agents_dir : NULL;
        if (agent_definition_apply(g_db, a->args_json, agent, adir, &cerr) != 0) {
            LOG_WARN_("create_agent apply failed: %s", cerr ? cerr : "?");
            *rename_failed = 1; /* generic apply-failed: error result, no grant */
        }
        free(cerr);
    } else if (strcmp(a->action, "update_agent") == 0) {
        char *cerr = NULL;
        if (agent_definition_update_apply(g_db, a->args_json, agent, &cerr) != 0) {
            LOG_WARN_("update_agent apply failed: %s", cerr ? cerr : "?");
            *rename_failed = 1; /* generic apply-failed: error result, no grant */
        }
        free(cerr);
    } else if (strcmp(a->action, "rename_agent") == 0) {
        char *nn = tool_args_str(g_db, a->args_json, "name");
        char *pr = tool_args_str(g_db, a->args_json, "preamble");
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
                /* Update live agent name */
                snprintf((char *)rctx->agent_name, 64, "%s", nn);
                setenv("CCLAW_AGENT_NAME", nn, 1);
            }
        }
rename_done:
        free(nn); free(pr);
    }
    /* No caps rebind here: the dispatch loop reloads caps from grants before
     * every tool batch, so the applied grants take effect on the next dispatch. */
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
                                         ApprovalDecision decision, const char *decided_via,
                                         int64_t grant_expires_at) {
    int approved = (decision != APPROVAL_DENY);
    int is_apply = a->resolve && strcmp(a->resolve, "apply") == 0;
    int expired = decided_via && strncmp(decided_via, "auto:", 5) == 0;
    if (is_apply) {
        if (approved && decision == APPROVAL_ALWAYS) {
            int rename_failed = 0;
            apply_grant(a, agent, &rename_failed, grant_expires_at);
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

void resolve_approval(int64_t approval_id, ApprovalDecision decision, const char *decided_via,
                      int64_t grant_expires_at) {
    int approved = (decision != APPROVAL_DENY);
    Approval *a = approval_resolve(g_db, approval_id, approved, decided_via);
    if (!a) return;

    int64_t session_id = a->session_id;
    const char *agent = session_get_agent_name(g_db, session_id);

    /* Block window already lapsed → deliver async, leave the turn untouched. */
    if (approval_is_post_window(session_id, a->tool_call_id)) {
        resolve_approval_post_window(a, agent, decision, decided_via, grant_expires_at);
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
            if (a->action && strcmp(a->action, "sensitive") == 0) {
                /* Sensitivity axis: no standing grant may satisfy a sensitive
                 * target — ALWAYS is coerced to ONCE (specs/trust.md). The
                 * approval still passes this one frozen call; the next call
                 * to the same target parks again. */
                LOG_INFO_("approval always coerced to once reason=sensitive tool=%s",
                          a->tool_name ? a->tool_name : "?");
            } else if (a->action && strcmp(a->action, "secret_bind") == 0) {
                /* Approve & bind: a durable binding needs a static target —
                 * only a url-carrying call has one. shell/js calls coerce to
                 * ONCE (nothing standing is minted from an unattributable
                 * target); pre-seed those with `cclaw secret-bind`. */
                char host[254];
                if (a->tool_name && strcmp(a->tool_name, "web_fetch") == 0 &&
                    a->args_json &&
                    web_args_url_host(a->args_json, host, sizeof(host))) {
                    size_t un = 0;
                    char **names = secret_placeholder_names(a->args_json, &un);
                    for (size_t i = 0; i < un; i++) {
                        if (db_secret_host_bind(g_db, names[i], host) == 0)
                            LOG_INFO_("secret binding recorded name=%s host=%s via=approval",
                                      names[i], host);
                    }
                    secret_names_free(names, un);
                } else {
                    LOG_INFO_("approval always coerced to once reason=secret_bind_no_static_target tool=%s",
                              a->tool_name ? a->tool_name : "?");
                }
            } else {
                /* "Allow and stop asking" — flip the standing mode to silent. */
                agent_config_set_tool_mode(g_db, agent, a->action, "silent");
            }
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
        apply_grant(a, agent, &rename_failed, grant_expires_at);

    /* Build tool result message */
    char result_buf[256];
    if (rename_failed)
        snprintf(result_buf, sizeof(result_buf), "error: %s failed, rolled back",
                 a->action ? a->action : "apply");
    else if (decision == APPROVAL_ALWAYS && a->action &&
             strcmp(a->action, "extension_promote") == 0)
        snprintf(result_buf, sizeof(result_buf),
                 "approved: extension_promote — registered; its tools are "
                 "granted to you and callable immediately");
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

/* Append `header` plus a fenced block of the rows yielded by sql (single
 * text column, one text bind) — or nothing when the query yields no rows.
 * The fence renders as monospace <pre> on chat channels so columns stay
 * aligned; in a terminal it reads fine as-is. */
static void append_enum_block(Buf *out, const char *header, const char *sql,
                              const char *bind) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, bind, -1, SQLITE_STATIC);
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (!v) continue;
        if (n++ == 0) buf_appendf(out, "%s\n```\n", header);
        buf_appendf(out, "%s\n", v);
    }
    if (n) buf_append_str(out, "```\n");
    sqlite3_finalize(st);
}

/* request_changes enumerations, grouped by scope: the approver must see
 * whether a line changes only the asking agent or the whole system. Every
 * requested VALUE is listed (which hosts, which paths) — never counts. */
static const char *SQL_CHANGES_AGENT_SCOPED =
    "SELECT format('%-7s%s','tool',atom) FROM json_each(?1,'$.changes.grants.tools')"
    " UNION ALL SELECT format('%-7s%s','host',atom) FROM json_each(?1,'$.changes.grants.hosts')"
    " UNION ALL SELECT format('%-7s%s','read',atom) FROM json_each(?1,'$.changes.grants.read_paths')"
    " UNION ALL SELECT format('%-7s%s','write',atom) FROM json_each(?1,'$.changes.grants.write_paths')"
    " UNION ALL SELECT format('%-7s%s','route',atom) FROM json_each(?1,'$.changes.routes')"
    " UNION ALL SELECT format('%s = %s',key,atom) FROM json_each(?1,'$.changes.agent')";

static const char *SQL_CHANGES_SYSTEM_WIDE =
    "SELECT format('%s = %s',key,atom) FROM json_each(?1,'$.changes.config')"
    " UNION ALL SELECT format('provider %s -> %s (key from secret %s)',"
    "   json_extract(?1,'$.changes.provider.provider'),"
    "   json_extract(?1,'$.changes.provider.base_url'),"
    "   json_extract(?1,'$.changes.provider.api_key_env'))"
    " WHERE json_extract(?1,'$.changes.provider.provider') IS NOT NULL";

/* create_agent enumeration over the parked definition — values, not counts:
 * this is the approval that mints a new autonomous actor, so it must be the
 * most transparent one, not the least. */
static const char *SQL_CREATE_AGENT_LINES =
    "SELECT format('%-7s%s','tool',atom) FROM json_each(?1,'$.grants.tools')"
    " UNION ALL SELECT format('%-7s%s','host',atom) FROM json_each(?1,'$.grants.hosts')"
    " UNION ALL SELECT format('%-7s%s','read',atom) FROM json_each(?1,'$.grants.read_paths')"
    " UNION ALL SELECT format('%-7s%s','write',atom) FROM json_each(?1,'$.grants.write_paths')"
    " UNION ALL SELECT format('%-7s%s','ext',atom) FROM json_each(?1,'$.extensions')"
    " UNION ALL SELECT format('%-7s%s','model',json_extract(?1,'$.primary_model'))"
    "   WHERE json_extract(?1,'$.primary_model') IS NOT NULL"
    " UNION ALL SELECT format('%-7s%d block(s)','memory',json_array_length(?1,'$.memory_blocks'))"
    "   WHERE json_array_length(?1,'$.memory_blocks') > 0";

/* update_agent enumeration — grants add, scalar fields overwrite; long text
 * (system_prompt) is clipped per line, the whole-summary truncation below is
 * the backstop. */
static const char *SQL_UPDATE_AGENT_LINES =
    "SELECT format('%-7s%s','tool',atom) FROM json_each(?1,'$.grants.tools')"
    " UNION ALL SELECT format('%-7s%s','host',atom) FROM json_each(?1,'$.grants.hosts')"
    " UNION ALL SELECT format('%-7s%s','read',atom) FROM json_each(?1,'$.grants.read_paths')"
    " UNION ALL SELECT format('%-7s%s','write',atom) FROM json_each(?1,'$.grants.write_paths')"
    " UNION ALL SELECT format('%s = %s', key,"
    "     CASE WHEN length(atom) > 200 THEN substr(atom,1,200)||'...' ELSE atom END)"
    "   FROM json_each(?1)"
    "   WHERE key NOT IN ('name','grants','reason') AND type!='object'";

/* Render a human-readable markdown summary of an approval — never the raw
 * args_json blob. Grant-shaped approvals enumerate every requested value in
 * fenced blocks grouped by scope; any other tool's call arguments (shape
 * unknown in general) surface their most common salient field. Returns a
 * heap string, caller frees. */
static char *format_approval_summary(const Approval *a) {
    const char *args = a->args_json ? a->args_json : "{}";
    Buf out = {0};
    /* Collected extracted fields — freed in one sweep at the end. */
    char *f[3] = {0};
    if (a->tool_name && strcmp(a->tool_name, "request_config") == 0 &&
        a->action && strcmp(a->action, "request_changes") == 0) {
        buf_append_str(&out, "requests these changes:\n");
        append_enum_block(&out, "Agent-scoped (this agent only):",
                          SQL_CHANGES_AGENT_SCOPED, args);
        append_enum_block(&out, "System-wide:", SQL_CHANGES_SYSTEM_WIDE, args);
        f[0] = tool_args_str(g_db, args, "reason");
        if (f[0] && f[0][0]) buf_appendf(&out, "Reason: %s\n", f[0]);
    } else if (a->tool_name && strcmp(a->tool_name, "request_config") == 0 &&
               a->action && strcmp(a->action, "rename_agent") == 0) {
        f[0] = tool_args_str(g_db, args, "name");
        f[1] = tool_args_str(g_db, args, "reason");
        buf_appendf(&out, "rename_agent: %s", f[0] ? f[0] : "?");
        if (f[1] && f[1][0]) buf_appendf(&out, "\nReason: %s", f[1]);
    } else if (a->tool_name && strcmp(a->tool_name, "request_config") == 0) {
        buf_append_str(&out, a->action ? a->action : "?");
    } else if (a->tool_name && strcmp(a->tool_name, "extension_promote") == 0) {
        f[0] = tool_args_str(g_db, args, "name");
        f[1] = tool_args_str(g_db, args, "summary");
        buf_appendf(&out, "promote '%s': %s", f[0] ? f[0] : "?",
                    f[1] ? f[1] : "(no summary)");
    } else if (a->tool_name && strcmp(a->tool_name, "create_agent") == 0) {
        f[0] = tool_args_str(g_db, args, "name");
        f[1] = tool_args_str(g_db, args, "sandbox_profile");
        f[2] = tool_args_str(g_db, args, "clone_from");
        buf_appendf(&out, "wants to create agent **%s** (profile %s%s%s):\n",
                    f[0] ? f[0] : "?", f[1] ? f[1] : "default",
                    f[2] ? ", clone of " : "", f[2] ? f[2] : "");
        append_enum_block(&out, "Capabilities (all within the creator's own):",
                          SQL_CREATE_AGENT_LINES, args);
    } else if (a->tool_name && strcmp(a->tool_name, "update_agent") == 0) {
        f[0] = tool_args_str(g_db, args, "name");
        f[1] = tool_args_str(g_db, args, "reason");
        buf_appendf(&out, "wants to update agent **%s**:\n", f[0] ? f[0] : "?");
        append_enum_block(&out, "Changes (grants add; fields overwrite):",
                          SQL_UPDATE_AGENT_LINES, args);
        if (f[1] && f[1][0]) buf_appendf(&out, "Reason: %s\n", f[1]);
    } else {
        char *salient = tool_args_str(g_db, args, "command");
        if (!salient) salient = tool_args_str(g_db, args, "url");
        if (!salient) salient = tool_args_str(g_db, args, "path");
        f[0] = salient;
        if (salient)
            buf_appendf(&out, "%s: %s", a->tool_name ? a->tool_name : "?", salient);
        else
            buf_appendf(&out, "%s (%s)", a->tool_name ? a->tool_name : "?",
                        a->action ? a->action : "?");
    }
    for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++) free(f[i]);

    char *s = buf_take(&out);
    if (!s) return strdup("?");
    /* Telegram counts parsed text against its 4096 limit and buttoned
     * prompts are never chunked — truncate a huge document at a line break.
     * The appended fence closes a cut-open block; if the cut landed outside
     * a block the stray fence stays literal (unbalanced markers pass
     * through the renderer untouched), which is safe. */
    if (strlen(s) > 3500) {
        size_t cut = 3400;
        while (cut > 0 && s[cut] != '\n') cut--;
        if (cut == 0) {
            /* No line break at all (e.g. one huge single-line field) — hard
             * cut rather than emptying the summary; keep UTF-8 sequences
             * whole so the platform doesn't reject the text. */
            cut = 3400;
            while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
        }
        s[cut] = '\0';
        Buf t = {0};
        buf_appendf(&t, "%s\n```\n... (truncated)", s);
        free(s);
        s = buf_take(&t);
        if (!s) return strdup("?");
    }
    return s;
}

/* ── handle_approval_park: prompt the approver ────────────────── */

static void handle_approval_park(int64_t session_id) {
    Approval *a = approval_get_pending(g_db, session_id);
    if (!a) return;

    if (g_mode == 0) {
        /* CLI mode */
        if (g_auto_approve) {
            /* --auto-approve: answer immediately, never prompt. "rerun"
             * approvals get a single-use ONCE; "apply" grants (ambient
             * capabilities have no once semantics — see resolve_approval)
             * get ALWAYS with a self-expiring deadline so no durable config
             * is left behind. Applies to any session in the CLI tree,
             * including sub-agents. */
            int is_apply = a->resolve && strcmp(a->resolve, "apply") == 0;
            if (is_apply) {
                int64_t expires = time(NULL) + approval_timeout_seconds();
                resolve_approval(a->id, APPROVAL_ALWAYS, "cli:auto-approve", expires);
            } else {
                resolve_approval(a->id, APPROVAL_ONCE, "cli:auto-approve", 0);
            }
            approval_free(a);
            return;
        }
        if (!isatty(STDIN_FILENO)) {
            /* Non-interactive (-p mode): auto-deny */
            resolve_approval(a->id, APPROVAL_DENY, "auto:no-approver", 0);
            approval_free(a);
            return;
        }
        /* Interactive: prompt user. Name the asking session when it's a
         * sub-agent (session_id != g_cli_session) — otherwise a launch_agent
         * child's approval request is indistinguishable from the root's. */
        fprintf(stdout, "\n\033[1mApproval required\033[0m");
        if (session_id != g_cli_session) {
            char *sub_agent = session_get_agent_name(g_db, session_id);
            fprintf(stdout, " (sub-agent %s, session %lld)",
                    sub_agent ? sub_agent : "?", (long long)session_id);
            free(sub_agent);
        }
        {
            char *summary = format_approval_summary(a);
            fprintf(stdout, ": %s", summary ? summary : "?");
            free(summary);
        }
        if (a->resolve && strcmp(a->resolve, "apply") == 0)
            fprintf(stdout, "\nGrant? (y/n): ");
        else
            fprintf(stdout, "\nApprove? (y=always / o=once / n=no): ");
        fflush(stdout);
        g_cli_turn_active = 0;  /* unblock input loop for the y/n read */
        /* The actual y/n is read back in the main CLI input loop. */
        approval_free(a);
        return;
    }

    /* Daemon mode: enqueue outbox prompt if session has channel, else auto-deny.
     * channel_name/chat_id are read and finalized *before* the outbox
     * INSERT so this connection never holds an open SELECT across a write —
     * see specs note on read->write snapshot upgrades (instant SQLITE_BUSY). */
    const char *sql = "SELECT channel_name, chat_id FROM sessions WHERE id=?;";
    sqlite3_stmt *stmt;
    int has_channel = 0;
    char ch_name[64] = {0};
    char ch_id[64] = {0};
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, session_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *ch = (const char *)sqlite3_column_text(stmt, 0);
            const char *cid = (const char *)sqlite3_column_text(stmt, 1);
            if (ch && ch[0]) {
                has_channel = 1;
                snprintf(ch_name, sizeof(ch_name), "%s", ch);
                snprintf(ch_id, sizeof(ch_id), "%s", cid ? cid : "0");
            }
        }
        sqlite3_finalize(stmt);
    }
    if (has_channel) {
        /* Route to the channel's configured admin destination(s), not the
         * requesting session's own chat — a session may be exposed on a
         * channel/chat where the human watching it isn't the one who should
         * decide grants (e.g. a sub-agent surfaced on a groupchat). Falls
         * back to the session's own chat_id when no admin_ids are
         * configured, so the common 1:1 setup needs no extra config. */
        char *admins = channel_config_get(g_db, ch_name, "admin_ids");
        if (admins && !admins[0]) { free(admins); admins = NULL; }

        char *agent = session_get_agent_name(g_db, session_id);
        char *summary = format_approval_summary(a);
        Buf pb = {0};
        /* The approval id is part of the prompt text, not just the button
         * payload: a channel without inline buttons (Discord) decides with
         * "/approve <id>", and the admin must see the id and the full summary
         * in the same message they are deciding from. */
        buf_appendf(&pb, "**Approval #%lld** — agent %s (session %lld, channel %s):\n%s"
                         "\nDecide here: /approve %lld or /deny %lld",
                    (long long)a->id, agent ? agent : "?",
                    (long long)session_id, ch_name, summary ? summary : "?",
                    (long long)a->id, (long long)a->id);
        free(agent);
        free(summary);
        char *prompt = buf_take(&pb);
        if (!prompt) prompt = strdup("approval requested");

        /* "apply" approvals (request_config grants) have no once semantics —
         * resolve_approval rejects APPROVAL_ONCE for them outright (see the
         * apply-branch check above) — so don't offer a button that always
         * dead-ends in an error. Plain tool-call ("rerun") approvals keep it. */
        char keyboard[256];
        int is_apply = a->resolve && strcmp(a->resolve, "apply") == 0;
        if (is_apply) {
            snprintf(keyboard, sizeof(keyboard),
                     "[[{\"text\":\"Grant\",\"callback_data\":\"appr:%lld:yes\"},"
                     "{\"text\":\"Deny\",\"callback_data\":\"appr:%lld:no\"}]]",
                     (long long)a->id, (long long)a->id);
        } else {
            snprintf(keyboard, sizeof(keyboard),
                     "[[{\"text\":\"Approve\",\"callback_data\":\"appr:%lld:yes\"},"
                     "{\"text\":\"Once\",\"callback_data\":\"appr:%lld:once\"},"
                     "{\"text\":\"Deny\",\"callback_data\":\"appr:%lld:no\"}]]",
                     (long long)a->id, (long long)a->id, (long long)a->id);
        }

        /* Two shapes: the admin fan-out addresses *user* ids (to_user=1, the
         * handler resolves a DM), the fallback addresses the session's own
         * chat id. See channel_notify_admins. */
        const char *ins_sql =
            "INSERT INTO channel_outbox(channel_name, session_id, payload)"
            " VALUES(?1, ?2, json_object('chat_id', ?3, 'text', ?4,"
            " 'keyboard', json(?5), 'to_user', 1));";
        const char *ins_chat_sql =
            "INSERT INTO channel_outbox(channel_name, session_id, payload)"
            " VALUES(?1, ?2, json_object('chat_id', ?3, 'text', ?4, 'keyboard', json(?5)));";
        if (admins && admins[0]) {
            char *ids[CHANNEL_ADMIN_IDS_MAX];
            int n_ids = split_and_trim(admins, ids, CHANNEL_ADMIN_IDS_MAX);
            for (int i = 0; i < n_ids; i++) {
                sqlite3_stmt *ins;
                if (sqlite3_prepare_v2(g_db, ins_sql, -1, &ins, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(ins, 1, ch_name, -1, SQLITE_STATIC);
                    sqlite3_bind_int64(ins, 2, session_id);
                    sqlite3_bind_text(ins, 3, ids[i], -1, SQLITE_STATIC);
                    sqlite3_bind_text(ins, 4, prompt, -1, SQLITE_STATIC);
                    sqlite3_bind_text(ins, 5, keyboard, -1, SQLITE_STATIC);
                    sqlite3_step(ins); sqlite3_finalize(ins);
                }
            }
        } else {
            sqlite3_stmt *ins;
            if (sqlite3_prepare_v2(g_db, ins_chat_sql, -1, &ins, NULL) == SQLITE_OK) {
                sqlite3_bind_text(ins, 1, ch_name, -1, SQLITE_STATIC);
                sqlite3_bind_int64(ins, 2, session_id);
                sqlite3_bind_text(ins, 3, ch_id, -1, SQLITE_STATIC);
                sqlite3_bind_text(ins, 4, prompt, -1, SQLITE_STATIC);
                sqlite3_bind_text(ins, 5, keyboard, -1, SQLITE_STATIC);
                sqlite3_step(ins); sqlite3_finalize(ins);
            }
        }
        free(admins);
        free(prompt);
        if (g_cfg && g_cfg->db_path) channel_outbox_wake(g_cfg->db_path, ch_name);
    }
    if (!has_channel) {
        /* No channel binding — fail-closed: auto-deny */
        resolve_approval(a->id, APPROVAL_DENY, "auto:no-approver", 0);
    }
    approval_free(a);
}

/* ── event_step: shared event handling for both daemon and CLI loops ── */

static void reap_children(void);

/* postAdvance hooks: fire on the worker-completion wake, before run_advance
 * consumes the llm_running state. The state guard filters compaction
 * completions riding the same pipe; error entries are skipped (nothing for a
 * redact/annotate hook to act on, and error filtering drops them from context anyway). */
static void maybe_dispatch_post_advance(int64_t session_id) {
    if (!g_tool_setup ||
        g_tool_setup->ext_ctx.hooks[HOOK_POST_ADVANCE].count == 0)
        return;

    int64_t entry_id = -1;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(g_db,
            "SELECT e.id FROM entries e"
            " WHERE e.id = (SELECT MAX(id) FROM entries"
            "                WHERE session_id=?1 AND type='assistant_message')"
            "   AND e.stop_reason != 4"  /* STOP_REASON_ERROR */
            "   AND (SELECT state FROM sessions WHERE id=?1) = 'llm_running';",
            -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(s, 1, session_id);
    if (sqlite3_step(s) == SQLITE_ROW)
        entry_id = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    if (entry_id < 0) return;

    char *cmds = hook_dispatch_post_advance(&g_tool_setup->ext_ctx, g_db,
                                            session_id, entry_id);
    if (cmds) {
        hook_apply_post_advance(g_db, session_id, entry_id, cmds);
        free(cmds);
    }
}

static void event_step_worker(int worker_fd) {
    (void)worker_fd;
    int64_t completed_sid;
    while (llm_worker_read(&completed_sid) == 0) {
        if (completed_sid == -1) continue;
        maybe_dispatch_post_advance(completed_sid);
        run_advance(completed_sid);
    }
}

/* Drain tool-thread completion notifications: a finished EXEC_THREAD tool wrote
 * its result + completed its call with its own db, then pushed session_id here.
 * Advance on the poll thread exactly like worker/child completions. */
static void event_step_tool_thread(void) {
    int64_t completed_sid;
    while (tool_thread_read(&completed_sid) == 0) {
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

static const char *advance_action_name(AdvanceResult a) {
    switch (a) {
    case ADVANCE_NOOP:           return "noop";
    case ADVANCE_DISPATCH_LLM:   return "dispatch_llm";
    case ADVANCE_DISPATCH_TOOLS: return "dispatch_tools";
    case ADVANCE_DONE:           return "done";
    case ADVANCE_WAITING:        return "waiting";
    case ADVANCE_ERROR:          return "error";
    }
    return "?";
}

static void run_advance(int64_t session_id) {
    int max_iter = session_max_iter(session_id);
    /* Tag every log line from here — including advance_session's own state
     * transitions and the dispatch/deliver calls below, which run on this
     * thread — with the advancing session (and, once known, agent), so
     * support logs tie back to the conversation and its llm_responses rows. */
    cclaw_log_set_ctx(session_id, -1, NULL);
    AdvanceOutput out = advance_session(g_db, session_id, max_iter);
    LOG_INFO_("advance action=%s iter=%d",
              advance_action_name(out.action), out.iteration);
    cclaw_log_set_ctx(session_id, -1, out.agent_name);

    switch (out.action) {
    case ADVANCE_DISPATCH_LLM:
        if (dispatch_llm_req(session_id, out.agent_name, out.iteration) < 0) {
            /* Only the root CLI session drives the prompt; sub-agents advance
             * silently in the background. */
            if (g_mode == 0 && session_id == g_cli_session) g_cli_turn_active = 0;
        }
        break;
    case ADVANCE_DISPATCH_TOOLS: {
        /* dispatch_tool returns: 1 = inline (result written),
         * 3 = async parallel-safe dispatched (keep going), 0 = async serial
         * dispatched (stop and wait), 2 = parked for approval, <0 = failure.
         * Parallel-safe calls are launched back-to-back; a serial async call
         * (or a park/failure) stops dispatch and we wait for its completion. */
        /* The shared setup serves whichever session is advancing — root or any
         * sub-agent (in the daemon, many agents through one setup). This is the
         * ONLY caps/containment binding point: every batch reloads the
         * advancing agent's grants, sandbox_profile, and shell_path from the
         * DB, so the in-memory snapshot can never go stale (expiry, revoke,
         * rename, update_agent, agent switch). */
        if (g_tool_setup)
            agent_setup_refresh_caps(g_tool_setup, g_db, out.agent_name);
        int async_in_flight = 0;
        int stop = 0;
        for (int i = 0; i < out.tc_count && !stop; i++) {
            /* CAS claim (pending → running) before any tool logic runs, so a
             * co-pointed process (daemon + CLI on the same session) can't
             * double-dispatch the same call — the loser skips, the winner
             * owns the call's lifecycle. Paths that don't dispatch after all
             * (approval park, child ceiling) unclaim below so the approval /
             * freed-slot re-advance can re-dispatch. */
            int claim = db_tool_call_claim(g_db, session_id, out.calls[i].call_id);
            if (claim == 0) {
                LOG_INFO_("tool claim lost tool=%s (co-pointed dispatcher)",
                          out.calls[i].name);
                continue;
            }
            if (claim < 0) { stop = 1; break; }  /* db error — retry on re-advance */
            int rc = dispatch_tool(session_id, out.agent_name, &out.calls[i]);
            switch (rc) {
            case 1: break;                              /* inline done — next */
            case 3: async_in_flight = 1; break;         /* parallel async — next */
            case 0: async_in_flight = 1; stop = 1; break; /* serial async — wait */
            default:                                    /* parked (2) or failure */
                /* The call didn't dispatch: release the claim (a path that
                 * already wrote a result + 'done' is left alone — CAS). */
                db_tool_call_unclaim(g_db, session_id, out.calls[i].call_id);
                /* -1 = child ceiling hit: the call is back to 'pending' and un-
                 * forked. Remember the session so a freed slot (reap) re-advances
                 * it instead of leaving it stuck in tool_running forever. */
                if (rc == -1) stalled_add(session_id);
                stop = 1;
                break;
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
        /* In daemon mode this used to vanish silently — log it so a stuck
         * session leaves a trail the user can send. */
        LOG_ERROR_("advance_session failed");
        if (g_mode == 0) {
            fprintf(stderr, "error: session advance failed\n");
            if (session_id == g_cli_session) g_cli_turn_active = 0;
        }
        break;
    }
    cclaw_log_clear_ctx();
}

static void deliver_response(int64_t session_id) {
    if (g_mode == 0) {
        /* CLI stdout belongs to the root session's turn. A sub-agent finishing
         * routes its result to the parent's tool_call (advance.c), not stdout. */
        if (session_id != g_cli_session) return;
        cli_indicator_clear();
        char *text = get_response_text(g_db, session_id);
        if (text) { printf("%s\n", text); free(text); }
        g_cli_turn_active = 0;
        return;
    }

    /* Daemon mode: read channel from session, write outbox */
    const char *src_sql = "SELECT channel_name, chat_id FROM sessions WHERE id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(g_db, src_sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(stmt, 1, session_id);
    char *channel = NULL, *chat_id = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *s = (const char *)sqlite3_column_text(stmt, 0);
        if (s) channel = strdup(s);
        s = (const char *)sqlite3_column_text(stmt, 1);
        if (s) chat_id = strdup(s);
    }
    sqlite3_finalize(stmt);
    if (!channel) return;

    /* Delivery mode lives on the route (no route = auto): 'explicit' keeps
     * turn output in the session — the agent speaks only via channel_send.
     * Group listen-and-decide depends on this. */
    {
        const char *msql =
            "SELECT delivery_mode FROM channel_routes"
            " WHERE channel_name=?1 AND chat_id=?2;";
        sqlite3_stmt *ms;
        int explicit_mode = 0;
        if (sqlite3_prepare_v2(g_db, msql, -1, &ms, NULL) == SQLITE_OK) {
            sqlite3_bind_text(ms, 1, channel, -1, SQLITE_STATIC);
            sqlite3_bind_text(ms, 2, chat_id ? chat_id : "0", -1, SQLITE_STATIC);
            if (sqlite3_step(ms) == SQLITE_ROW) {
                const char *m = (const char *)sqlite3_column_text(ms, 0);
                explicit_mode = m && strcmp(m, "explicit") == 0;
            }
            sqlite3_finalize(ms);
        }
        if (explicit_mode) { free(channel); free(chat_id); return; }
    }

    /* Ingestion-only channel (handler defines no onOutbox, recorded in
     * channel_state at load time): nothing would ever drain the row. */
    {
        const char *osql =
            "SELECT value FROM channel_state"
            " WHERE channel_name=?1 AND key='has_outbox';";
        sqlite3_stmt *os;
        int ingest_only = 0;
        if (sqlite3_prepare_v2(g_db, osql, -1, &os, NULL) == SQLITE_OK) {
            sqlite3_bind_text(os, 1, channel, -1, SQLITE_STATIC);
            if (sqlite3_step(os) == SQLITE_ROW) {
                const char *v = (const char *)sqlite3_column_text(os, 0);
                ingest_only = v && strcmp(v, "0") == 0;
            }
            sqlite3_finalize(os);
        }
        if (ingest_only) { free(channel); free(chat_id); return; }
    }

    char *text = get_response_text(g_db, session_id);
    if (!text) { free(channel); free(chat_id); return; }

    /* Build outbox payload via SQLite json_object (safe escaping) */
    const char *ins_sql =
        "INSERT INTO channel_outbox(channel_name, session_id, payload)"
        " VALUES(?1, ?2, json_object('chat_id', ?3, 'text', ?4));";
    sqlite3_stmt *ins;
    if (sqlite3_prepare_v2(g_db, ins_sql, -1, &ins, NULL) == SQLITE_OK) {
        sqlite3_bind_text(ins, 1, channel, -1, SQLITE_STATIC);
        sqlite3_bind_int64(ins, 2, session_id);
        sqlite3_bind_text(ins, 3, chat_id ? chat_id : "0", -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 4, text, -1, SQLITE_STATIC);
        sqlite3_step(ins);
        sqlite3_finalize(ins);

        if (g_cfg && g_cfg->db_path) channel_outbox_wake(g_cfg->db_path, channel);
    }

    free(text);
    free(channel);
    free(chat_id);
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

            /* Network provenance: a non-empty hosts tag marks the result as
             * untrusted external content. Strip invisible Unicode NOW so the
             * query-time wrap in llm_payload can't be broken out of.
             * Fail closed on a damaged signal: a truncated or oversized-
             * dropped meta means we can't prove the result had no network
             * exposure — sanitize as if it did, and record no hosts tag
             * (never a partial one). Only an explicit zero-length meta
             * (non-network tier / error frame) skips the strip. */
            char *hosts = c->hosts_json;
            int meta_damaged = c->frame_meta_len > 0 &&
                (!c->hosts_json || c->frame_meta_read != c->frame_meta_len);
            if (meta_damaged ||
                (hosts && (hosts[0] == '\0' || strcmp(hosts, "[]") == 0)))
                hosts = NULL;
            if (hosts || meta_damaged) {
                size_t slen = out_len;
                char *st = unicode_strip_invisible(output, slen, &slen);
                if (st) { free(output); output = st; }
                out_len = strlen(output);
            }
            /* (Forged fence markers are neutralized for ALL results —
             * network-tagged or not — inside tool_result_postprocess.) */

            /* Explicit capture first (raw result), then postprocess:
             * deinterpolate + scan/redact. Fresh per-call snapshot (same
             * rationale as dispatch_tool) — a secret born mid-session must
             * be maskable in a reaped sandboxed child's output too. */
            { char *cap = secret_capture_apply(g_db, c->tool_args, output);
              if (cap) { free(output); output = cap; out_len = strlen(output); } }
            { size_t snap_n = 0;
              ShellSecret *snap = secrets_snapshot(g_db,
                  g_tool_setup ? g_tool_setup->secrets : NULL,
                  g_tool_setup ? g_tool_setup->secret_count : 0, &snap_n);
              char *pp = tool_result_postprocess(output, snap, snap_n);
              secrets_snapshot_free(snap, snap_n);
              if (pp) { free(output); output = pp; out_len = strlen(output); } }

            /* afterToolCall hooks: chained result replacement, post-scanner,
             * pre-write (same contract as the inline dispatch path). The
             * replacement comes back already marker-sanitized (hook_dispatch
             * owns that invariant) — this reap path carries the
             * network_hosts-tagged results, so a transform hook must not be
             * able to smuggle a forged fence past storage-time sanitization. */
            char *hook_annotate = NULL;
            if (g_tool_setup) {
                char *rep = hook_dispatch_observe_tool_call(&g_tool_setup->ext_ctx,
                                g_db, c->tool_name, c->tool_args, output,
                                &hook_annotate);
                if (rep) { free(output); output = rep; out_len = strlen(output); }
            }

            /* CLI progress */
            if (g_mode == 0) {
                if (out_len <= 80)
                    fprintf(stdout, "\033[2m→ %s\033[0m\n", output);
                else
                    fprintf(stdout, "\033[2m→ %.77s...\033[0m\n", output);
                fflush(stdout);
            }

            /* Ensure valid UTF-8 before DB storage (binary tool output like
             * gzip would poison JSON serialization of the payload later). */
            { char *clean = utf8_sanitize(output, out_len);
              if (clean) { free(output); output = clean; out_len = strlen(output); } }

            /* Write result to DB */
            char *stored = truncate_and_spill(output, session_id, c->tool_call_id);
            ToolResult tr = {.tool_call_id = c->tool_call_id,
                             .content = stored ? stored : output};
            int is_err = (strncmp(output, "error:", 6) == 0);
            Message msg = {.role = ROLE_TOOL, .tool_result = &tr,
                           .tool_name = "", .is_error = is_err};
            int64_t rid = entry_append_with_turn(g_db, session_id, &msg, c->turn_id);
            if (hosts && rid > 0)
                db_entry_set_network_hosts(g_db, rid, hosts);
            db_tool_call_complete_with_result(g_db, c->entry_id, c->tool_call_id, rid);
            LOG_INFO_("tool done tool=%s is_err=%d", c->tool_name, is_err);
            LOG_TRACE_("tool result tool=%s len=%zu content=%s",
                       c->tool_name, out_len, output);
            if (hook_annotate) {
                if (rid > 0) hook_entry_data_patch(g_db, rid, hook_annotate);
                free(hook_annotate);
            }
            free(stored);
            free(output);
            c->outbuf = NULL;
            c->outbuf_len = 0;

            child_remove(c);
            run_advance(session_id);
        }
    }
    /* A tool slot may have freed — re-advance any ceiling-stalled sessions. */
    stalled_drain();
}

/* ── Helpers ────────────────────────────────────────────────────── */

static void print_usage(void) {
    printf("usage: cclaw [options]\n"
           "       cclaw install [--system]    set up a long-lived daemon (systemd unit)\n"
           "       cclaw uninstall [--system]  stop + remove that unit\n"
           "       cclaw sensitive add|rm <host> | list\n"
           "                                   label a target sensitive: every tool call\n"
           "                                   touching it parks for approval, no standing\n"
           "                                   grant applies, and the proxy denies it ambiently\n"
           "       cclaw secret-bind <name> <host> | rm <name> <host> | list\n"
           "                                   bind a secret to its legitimate hosts; a call\n"
           "                                   submitting the secret elsewhere parks, and a\n"
           "                                   secret-carrying shell/js call can reach ONLY\n"
           "                                   its bound hosts\n"
           "       cclaw secret set <NAME> [value] | rm <NAME> | list\n"
           "                                   mint/remove a DB-backed secret (no value arg\n"
           "                                   reads one line from stdin); born with zero host\n"
           "                                   bindings, so its first use always parks\n"
           "       cclaw route add <channel> <chat_id> <agent> | rm <channel> <chat_id> | list\n"
           "                                   bind a channel+chat to an agent; chat_id '*' is\n"
           "                                   the channel-wide default (else falls back to\n"
           "                                   default_agent)\n"
           "       cclaw channel [list] | swap <channel> <ext> | revert <channel> | restart <channel>\n"
           "                                   hot-swap the extension behind a live channel\n"
           "                                   (previous kept as revert target; a swap that\n"
           "                                   crash-loops auto-reverts and notifies admins)\n"
           "       cclaw dashboard             print the tokenized admin dashboard URL\n"
           "                                   (token minted on first daemon start)\n"
           "       cclaw backup [dest]         VACUUM INTO snapshot (default\n"
           "                                   <db>.backup.<timestamp>); safe on a live\n"
           "                                   daemon, refuses to overwrite\n"
           "       cclaw resp [<id> [req] | list [n]]\n"
           "                                   read the raw LLM response archive; bare form\n"
           "                                   shows the last failure (what \"[resp #N]\" in an\n"
           "                                   error message cites)\n"
           "\n"
           "modes (default: interactive CLI):\n"
           "  --daemon           run as daemon (telegram, web, cron)\n"
           "  --channel <name>   run one channel's event loop in this process\n"
           "                     (normally fork+exec'd by the daemon, not run by hand)\n"
           "  --channel <name> --check     validate: manifest + JS load + onInit(),\n"
           "                                no event loop; draft/broken -> validated on pass\n"
           "  --channel <name> --activate  validated -> active (daemon execs it next tick)\n"
           "  --channel <name> --harness <scenario.json>  offline fixture-replay test\n"
           "                                against a scratch DB (no real network/DB touched)\n"
           "\n"
           "options:\n"
           "  -p <prompt>        single-turn: send prompt, print response, exit\n"
           "  -s <id>            session id\n"
           "  --trust-host       no kernel sandbox, no rlimits, no egress proxy,\n"
           "                     full env — tool allowlist still applies (CLI\n"
           "                     only, ignored by --daemon)\n"
           "  --auto-approve     answer parked approvals without prompting;\n"
           "                     single-use (rerun tools run once, capability\n"
           "                     grants expire — nothing durable is left behind)\n"
           "  --new              create a new session\n"
           "  --log-level=LEVEL  set log level (error|info|debug|trace)\n"
           "  -v, --debug        debug logging (timing, context stats)\n"
           "  -vv, --trace       trace logging (full req/resp JSON)\n"
           "  --doctor           print redacted diagnostic report and exit\n"
           "  --help             show this help\n"
           "\n"
           "note: running as root weakens mount-based read-only enforcement\n"
           "within sandboxed children (namespace root maps to real root).\n");
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
    cli_indicator_show();
    run_advance(g_cli_session);
}

/* ── main ───────────────────────────────────────────────────────── */

/* PATH_MAX-sized buffers make gcc's -Wformat-truncation flag snprintfs in the
 * functions below as possibly truncating, even though the inputs never
 * approach that length; clang doesn't have this warning at all. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif


/* Bootstrap the default agent on a fresh DB. Shared by CLI and daemon —
 * a headless install (daemon-only, e.g. a channel-driven deploy) must not
 * depend on someone having run the CLI once to get a routable agent. */
static void ensure_default_agent(const char *base_dir) {
    int ac = 0; char **al = db_agent_list(g_db, &ac);
    if (!al || ac == 0) {
        char ws[PATH_MAX]; snprintf(ws, sizeof(ws), "%s/agents/Assistant/workspace/.keep", base_dir);
        util_ensure_parent_dir(ws);
        /* Create default agent */
        const char *agent_sql =
            "INSERT OR IGNORE INTO agents(name, system_prompt, sandbox_profile)"
            " VALUES('Assistant', ?, 'standard');"
            ;
        sqlite3_stmt *bs;
        if (sqlite3_prepare_v2(g_db, agent_sql, -1, &bs, NULL) == SQLITE_OK) {
            sqlite3_bind_text(bs, 1, TPL_DEFAULT_SYSTEM_PROMPT_MD, -1, SQLITE_STATIC);
            sqlite3_step(bs); sqlite3_finalize(bs);
        }
        /* Seed default tools as grants */
        agent_grant_defaults(g_db, "Assistant");
        /* Disabled heartbeat pulse row — visible/enable-able in cron_list. */
        cron_seed_heartbeat(g_db, "Assistant");
        /* default_agent needs no write — 'Assistant' is the registry default */
        /* Seed default memory blocks. Explicitly 'system' placement: identity
         * (who am I / who is the user) belongs in the system prompt, where it
         * changes rarely and stays in the cached prefix — unlike new blocks,
         * which default to the per-turn 'context' placement. */
        memory_block_create(g_db, "Assistant", "AGENT",
            "Your identity, capabilities, and operational notes. Update as you learn about yourself.",
            5000, "system");
        memory_block_create(g_db, "Assistant", "USER",
            "Information about the user: preferences, context, working style. Update as you learn.",
            5000, "system");
        /* AGENT gets one functional starter entry (it drives the self-naming
         * flow). USER starts empty — its description already states the block's
         * purpose, so a placeholder entry would just be noise the model has to
         * carry until it edits it away. */
        memory_entry_add(g_db, "Assistant", "AGENT",
            "You are CClaw. You do not have a name yet — ask the user what they would like to call you, then save it here with memory_edit.");
    }
    if (al) { for (int i = 0; i < ac; i++) free(al[i]); free(al); }
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
    { char *def = config_get(g_db, "default_agent");
      snprintf(daemon_agent, sizeof(daemon_agent), "%s", def ? def : "Assistant");
      free(def); }
    snprintf(g_agent_name, sizeof(g_agent_name), "%s", daemon_agent);
    char daemon_agents_dir[PATH_MAX];
    { char base[PATH_MAX]; snprintf(base, sizeof(base), "%s", db_path);
      char *sl = strrchr(base, '/'); if (sl) *sl = '\0'; else snprintf(base, sizeof(base), ".");
      snprintf(daemon_agents_dir, sizeof(daemon_agents_dir), "%s/agents", base);
      ensure_default_agent(base); }
    AgentSetup daemon_setup;
    agent_setup_init(&daemon_setup, g_db, 0, g_cfg, g_agent_name);
    daemon_setup.req_cfg_ctx.agents_dir = daemon_agents_dir;
    daemon_setup.req_cfg_ctx.agent_name = g_agent_name;
    g_tool_setup = &daemon_setup;

    printf("cclaw %s — daemon mode\n", CCLAW_VERSION);
    web_start(g_cfg, g_db, db_path);
    /* No heartbeat thread: the pulse is a seeded cron row (kind='heartbeat')
     * fired by cron_run_due off the db_periodic tick — one scheduler. */

    /* Start LLM worker threads */
    if (llm_worker_start(db_path, g_llm_threads) != 0) {
        LOG_ERROR_("daemon: failed to start LLM worker");
        config_free(g_cfg); db_close(g_db); free(db_path); return 1;
    }
    int daemon_worker_fd = llm_worker_fd();
    util_set_nonblock(daemon_worker_fd);

    /* Re-run media jobs (voice transcriptions) a crashed run left behind —
     * same crash-recovery idea as llm_jobs, except these rows are re-runnable
     * (input is in the row), so resubmit instead of delete. */
    llm_worker_resubmit_media(g_db);

    /* Fire-and-forget tool threads (EXEC_THREAD vehicle) */
    if (tool_thread_start(db_path, 0) != 0) {
        LOG_ERROR_("daemon: failed to start tool threads");
        llm_worker_stop();
        config_free(g_cfg); db_close(g_db); free(db_path); return 1;
    }
    int daemon_tool_fd = tool_thread_fd();

    /* Init wake pipe + FIFO */
    wake_init();
    int fifo_fd = wake_fifo_open(db_path);

    /* SIGCHLD self-pipe */
    if (pipe(g_chld_pipe) != 0) { perror("pipe"); return 1; }
    fcntl(g_chld_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(g_chld_pipe[1], F_SETFD, FD_CLOEXEC);
    util_set_nonblock(g_chld_pipe[0]); util_set_nonblock(g_chld_pipe[1]);
    { struct sigaction sa = {0}; sa.sa_handler = sigchld_handler;
      sigemptyset(&sa.sa_mask); sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
      sigaction(SIGCHLD, &sa, NULL); }

    /* Launch channel processes */
    channel_launch_all(g_db);

    /* Sweep events parked from a previous life. Consume is otherwise only
     * triggered by a FIFO wake, so anything left in channel_events at crash
     * or restart would wait for the *next* inbound message to be replayed. */
    channel_consume_events(g_db);

    /* poll() setup — sized for fixed fds + all children with result pipes */
    int max_pfds = 6 + CHILD_MAX;  /* chld, wake, fifo, worker, tool-thread, result pipes */
    struct pollfd *pfds = malloc(max_pfds * sizeof(struct pollfd));
    if (!pfds) { perror("malloc"); return 1; }

    /* Register in the liveness table so this daemon's in-flight sessions are
     * owner-stamped and never reclaimed by a peer's recovery. */
    if (process_register(g_db, "daemon", getpid(), g_instance_id, sizeof(g_instance_id)) != 0)
        LOG_ERROR_("daemon: process registration failed");
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
        int d_tool_idx = nfds;
        pfds[nfds].fd = daemon_tool_fd; pfds[nfds].events = POLLIN; nfds++;

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
        if (pfds[d_tool_idx].revents & POLLIN)
            event_step_tool_thread();

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
    tool_thread_stop();  /* drain in-flight tool threads before freeing state */
    channel_shutdown_all();
    web_stop();
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
                   int64_t session_id, int new_session, int host_mode, int auto_approve) {
    /* ── CLI mode ────────────────────────────────────────────────── */
    g_mode = 0;
    g_auto_approve = auto_approve;
    g_next_db_poll = time(NULL);

    if (!g_cfg->provider.api_key || !g_cfg->provider.api_key[0]) {
        fprintf(stderr, "error: no API key (set OPENROUTER_API_KEY)\n");
        config_free(g_cfg); db_close(g_db); free(db_path); return 1;
    }

    /* Derive base_dir for workspace */
    char *base_dir = strdup(db_path);
    { char *sl = strrchr(base_dir, '/'); if (sl) *sl = '\0'; else { free(base_dir); base_dir = strdup("."); } }

    ensure_default_agent(base_dir);

    /* Agent selection */
    char *agent_sel = NULL;
    if (prompt || !isatty(STDIN_FILENO)) {
        char *def = config_get(g_db, "default_agent");
        agent_sel = def ? def : strdup("Assistant");
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

    /* Inject agent config env vars (for forked children). -y skips kernel
     * isolation only — the agent's tool allowlist still applies. */
    {
        AgentConfig *ac = agent_config_load_db(g_db, g_agent_name);
        if (ac) {
            if (ac->tool_count > 0) {
                size_t len = 0; for (size_t i = 0; i < ac->tool_count; i++) len += strlen(ac->tools[i]) + 1;
                char *csv = malloc(len); if (csv) { csv[0] = '\0';
                    for (size_t i = 0; i < ac->tool_count; i++) { if (i) strcat(csv, ","); strcat(csv, ac->tools[i]); }
                    setenv("CCLAW_TOOLS", csv, 1); free(csv); }
            }
            agent_config_free(ac);
        }
    }
    if (host_mode) setenv("CCLAW_SANDBOX_PROFILE", "host", 1);
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
    agent_setup_init(&setup, g_db, 0, g_cfg, g_agent_name);
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
        LOG_WARN_("process registration failed kind=cli");
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
    util_set_nonblock(g_chld_pipe[0]);
    util_set_nonblock(g_chld_pipe[1]);
    { struct sigaction sa = {0}; sa.sa_handler = sigchld_handler;
      sigemptyset(&sa.sa_mask); sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
      sigaction(SIGCHLD, &sa, NULL); }

    /* ── poll() setup ──────────────────────────────────────────────── */
    /* Allocate for fixed fds + all children with result pipes */
    int cli_max_pfds = 5 + CHILD_MAX;  /* worker, stdin, chld, wake, tool-thread + pipes */
    struct pollfd *cli_pfds = malloc(cli_max_pfds * sizeof(struct pollfd));
    if (!cli_pfds) { perror("malloc"); return 1; }

    /* Start LLM worker */
    int worker_fd = -1;
    int worker_idx = -1;  /* slot index, recomputed each loop iteration */
    if (llm_worker_start(db_path, g_llm_threads) == 0) {
        worker_fd = llm_worker_fd();
        util_set_nonblock(worker_fd);
    } else {
        LOG_ERROR_("llm worker start failed");
        agent_setup_destroy(&setup); free(base_dir); config_free(g_cfg); db_close(g_db); free(db_path); return 1;
    }

    /* Fire-and-forget tool threads (EXEC_THREAD vehicle); cli_mode=1 prints the
     * dim "→ result" progress line like the inline path. */
    int cli_tool_fd = -1;
    if (tool_thread_start(db_path, 1) == 0) cli_tool_fd = tool_thread_fd();

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
        util_set_nonblock(STDIN_FILENO);
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
        int tool_idx = -1;
        if (cli_tool_fd >= 0) {
            tool_idx = cli_nfds;
            cli_pfds[cli_nfds].fd = cli_tool_fd; cli_pfds[cli_nfds].events = POLLIN; cli_nfds++;
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
                cli_prompt();
            }
        }
        if (worker_idx >= 0 && (cli_pfds[worker_idx].revents & POLLIN)) {
            event_step_worker(worker_fd);

            if (g_mode == 0 && !g_cli_turn_active) {
                if (g_cli_done) goto done;
                cli_prompt();
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
                cli_prompt();
            }
        }
        if (tool_idx >= 0 && (cli_pfds[tool_idx].revents & POLLIN)) {
            event_step_tool_thread();
            if (g_mode == 0 && !g_cli_turn_active) {
                if (g_cli_done) goto done;
                cli_prompt();
            }
        }
        if (stdin_idx >= 0 && (cli_pfds[stdin_idx].revents & (POLLERR | POLLNVAL))) {
            /* stdin fd broken/closed — exit instead of spinning on POLLNVAL */
            LOG_ERROR_("stdin: poll error (revents=0x%x), exiting",
                       cli_pfds[stdin_idx].revents);
            goto done;
        }
        if (stdin_idx >= 0 && (cli_pfds[stdin_idx].revents & POLLIN)) {
            if (g_cli_turn_active) {
                LOG_DEBUG_("stdin: POLLIN but turn active, skipping");
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
            LOG_DEBUG_("stdin: read line [%s]", linebuf);

            if (!linebuf[0]) { linepos = 0; cli_prompt(); continue; }
            if (strcmp(linebuf, "exit") == 0 || strcmp(linebuf, "quit") == 0) goto done;

            /* Check if we're waiting for an approval decision — anywhere in
             * the CLI's session subtree, not just the root (a sub-agent's
             * request_config parks the same way and needs the same y/n). */
            {
                Approval *pa = approval_get_pending_subtree(g_db, g_cli_session);
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
                    resolve_approval(pa->id, d, "cli:interactive", 0);
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
    tool_thread_stop();  /* drain in-flight tool threads before freeing state */
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
    /* --run-tool: early intercept for sandboxed file tool child. No DB, no key,
     * no config. The child reads its request from fd 3. */
    if (argc >= 2 && strcmp(argv[1], "--run-tool") == 0) return run_tool_main();

    /* --doctor: one-shot diagnostic bundle. Runs its own DB/config path so a
     * broken DB open is itself a finding — never bail on failures. */
    if (argc >= 2 && strcmp(argv[1], "--doctor") == 0) return doctor_main();

    /* install / uninstall: bare words (not flags) — the onboarding verb, not
     * a mode switch. No DB/config touched; writes go straight to the
     * filesystem + systemd. */
    if (argc >= 2 && strcmp(argv[1], "install") == 0) return install_main(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "uninstall") == 0) return uninstall_main(argc, argv);

    /* sensitive / secret-bind: operator verbs for the two escalation rules
     * (specs/trust.md). Deliberately CLI-only — no agent tool sets these. */
    if (argc >= 2 && strcmp(argv[1], "sensitive") == 0) return sensitive_main(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "secret-bind") == 0) return secret_bind_main(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "secret") == 0) return secret_main(argc, argv);

    /* route: operator verb binding a channel+chat to an agent (channel_routes).
     * CLI-only, same rationale as sensitive/secret-bind — an authority change. */
    if (argc >= 2 && strcmp(argv[1], "route") == 0) return route_main(argc, argv);

    /* channel: operator verbs for the extension hot-swap flow (list/swap/
     * revert/restart). CLI-only — swapping channel code is an authority change. */
    if (argc >= 2 && strcmp(argv[1], "channel") == 0) return channel_cli_main(argc, argv);

    /* resp: read the llm_responses forensic archive ("[resp #N]" in error
     * messages). Read-only; exists because JSONB bodies need SQLite >= 3.45
     * and target boxes ship older system CLIs. */
    if (argc >= 2 && strcmp(argv[1], "backup") == 0) return backup_main(argc, argv);

    if (argc >= 2 && strcmp(argv[1], "resp") == 0) return resp_main(argc, argv);

    /* dashboard: print the tokenized /admin URL (token minted at daemon start). */
    if (argc >= 2 && strcmp(argv[1], "dashboard") == 0) return dashboard_main();

    int daemon_mode = 0, new_session = 0, host_mode = 0, auto_approve = 0;
    int channel_check = 0, channel_activate_flag = 0;
    const char *channel_mode = NULL;
    const char *channel_harness_scenario = NULL;
    LogLevel log_level_override = LOG_LEVEL_INFO;
    int log_level_set = 0;
    int64_t session_id = -1;
    const char *prompt = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) { print_usage(); return 0; }
        else if (strcmp(argv[i], "--version") == 0) {
            printf("cclaw %s (%s)\n", VERSION_COMMIT, BUILD_DATE);
            return 0;
        }
        else if (strcmp(argv[i], "--daemon") == 0) daemon_mode = 1;
        else if (strcmp(argv[i], "--channel") == 0) { if (++i >= argc) { fprintf(stderr, "--channel requires name\n"); return 1; } channel_mode = argv[i]; }
        else if (strcmp(argv[i], "--check") == 0) channel_check = 1;
        else if (strcmp(argv[i], "--activate") == 0) channel_activate_flag = 1;
        else if (strcmp(argv[i], "--harness") == 0) { if (++i >= argc) { fprintf(stderr, "--harness requires a scenario path\n"); return 1; } channel_harness_scenario = argv[i]; }
        else if (strncmp(argv[i], "--log-level=", 12) == 0) { log_level_override = log_level_parse(argv[i]+12); log_level_set = 1; }
        else if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-v") == 0) { log_level_override = LOG_LEVEL_DEBUG; log_level_set = 1; }
        else if (strcmp(argv[i], "--trace") == 0 || strcmp(argv[i], "-vv") == 0) { log_level_override = LOG_LEVEL_TRACE; log_level_set = 1; }
        else if (strcmp(argv[i], "--new") == 0) new_session = 1;
        else if (strcmp(argv[i], "--trust-host") == 0) host_mode = 1;
        else if (strcmp(argv[i], "--auto-approve") == 0) auto_approve = 1;
        else if (strncmp(argv[i], "--llm-threads=", 14) == 0) g_llm_threads = atoi(argv[i]+14);
        else if (strcmp(argv[i], "-p") == 0) { if (++i >= argc) { fprintf(stderr, "-p requires arg\n"); return 1; } prompt = argv[i]; }
        else if (strcmp(argv[i], "-s") == 0) { if (++i >= argc) { fprintf(stderr, "-s requires arg\n"); return 1; } session_id = atoll(argv[i]); }
        else if (strncmp(argv[i], "--session-id=", 13) == 0) session_id = atoll(argv[i]+13);
        else { fprintf(stderr, "unknown option: %s\n", argv[i]); return 1; }
    }

    if (host_mode && daemon_mode)
        fprintf(stderr, "warning: --trust-host is ignored by --daemon; daemon agents "
                "sandbox per their sandbox_profile\n");

    if ((channel_check || channel_activate_flag || channel_harness_scenario) && !channel_mode) {
        fprintf(stderr, "--check / --activate / --harness require --channel <name>\n");
        return 1;
    }
    if ((channel_check ? 1 : 0) + (channel_activate_flag ? 1 : 0) + (channel_harness_scenario ? 1 : 0) > 1) {
        fprintf(stderr, "--check, --activate, and --harness are mutually exclusive\n");
        return 1;
    }

    {   const char *v = getenv("CCLAW_LLM_THREADS");
        if (v && atoi(v) > 0) g_llm_threads = atoi(v);
    }

    /* CLI tees logs to stderr so the interactive session sees them inline;
     * daemon and channel-runner children log strictly to syslog/journald. */
    int cli_tty = (!daemon_mode && channel_mode == NULL);
    cclaw_log_init(cli_tty);
    /* Provisional level so writes between here and the post-config
     * cclaw_log_set_level below aren't wide open (DB open, schema, seeding
     * all log). Refined once config resolves. */
    cclaw_log_set_level(log_level_set ? log_level_override
                                      : log_level_parse(getenv("CCLAW_LOG_LEVEL")));

    shutdown_init();
    crash_handler_init();

    /* ── Open DB ─────────────────────────────────────────────────── */
    db_configure_logging();   /* before the first db_open (sqlite3_initialize) */
    char *db_path = util_resolve_db_path();
    util_ensure_parent_dir(db_path);
    g_db = db_open(db_path);
    if (!g_db) { fprintf(stderr, "cannot open DB: %s\n", db_path); free(db_path); return 1; }

    if (!db_schema_compat(g_db)) {
        fprintf(stderr,
            "error: %s cannot be upgraded to schema v%d.\n"
            "The DB may be from a future build or too old. Delete it and restart:\n"
            "  rm %s %s-wal %s-shm\n",
            db_path, CCLAW_SCHEMA_VERSION, db_path, db_path, db_path);
        db_close(g_db); free(db_path); return 1;
    }

    /* Schema + seed in one exclusive transaction. When multiple processes
     * start concurrently (daemon + CLI), the first grabs the write lock and
     * does all DDL + seeding atomically; the others wait (busy handler) then
     * find everything already done (IF NOT EXISTS / COUNT>0 checks). */
    sqlite3_exec(g_db, "BEGIN EXCLUSIVE", NULL, NULL, NULL);
    if (db_ensure_schema(g_db) != 0) {
        sqlite3_exec(g_db, "ROLLBACK", NULL, NULL, NULL);
        db_close(g_db); free(db_path); return 1;
    }
    db_seed_defaults(g_db);
    config_registry_sync(g_db);
    sqlite3_exec(g_db, "COMMIT", NULL, NULL, NULL);

    { uint8_t sk[32]; if (secret_key_load_or_create(db_path, sk) == 0) db_set_secret_key(sk); }

    /* Reinstalls the shipped bundles on every start (files are a cache of
     * the binary's templates); idempotent against a current DB. */
    extension_install_builtin(g_db, db_path);

    g_cfg = config_load(g_db);
    if (!g_cfg) { fprintf(stderr, "config load failed\n"); db_close(g_db); return 1; }
    if (log_level_set) {
        g_cfg->log_level = log_level_override;
        setenv("CCLAW_LOG_LEVEL", log_level_name(log_level_override), 1);
    } else if (cli_tty && !getenv("CCLAW_LOG_LEVEL")) {
        /* Interactive CLI defaults to errors-only on the tty so logs don't
         * clutter the conversation; -v/-vv or CCLAW_LOG_LEVEL opt back in.
         * (The daemon keeps the info default.) */
        g_cfg->log_level = LOG_LEVEL_ERROR;
    }
    cclaw_log_set_level(g_cfg->log_level);
    snprintf(g_log_level_env, sizeof g_log_level_env, "CCLAW_LOG_LEVEL=%s",
             log_level_name(g_cfg->log_level));

    /* Enable SQLite query profiling at debug level and above */
    db_set_slow_query_ms(config_get_int(g_db, "sql_slow_ms"));
    if (g_cfg->log_level >= LOG_LEVEL_DEBUG)
        db_enable_trace(g_db);

    /* ── Channel lifecycle gate ──────────────────────────────────────
     * --activate (validated→active) is a pure DB transition, no JS touched —
     * reuses the already-open g_db. --check runs the same JS-load + onInit()
     * sequence the live runner does (via channel_runner_check), then this
     * process (not the runner) applies the draft/broken→validated transition
     * on success — keeping "did it pass" and "did we record that" separate. */
    if (channel_mode && channel_activate_flag) {
        int rc = channel_activate(g_db, channel_mode);
        if (rc == 0) {
            printf("channel '%s': validated -> active\n", channel_mode);
        } else {
            char *cur = channel_get_status(g_db, channel_mode);
            fprintf(stderr, "error: cannot activate '%s' (status=%s, need 'validated')\n",
                    channel_mode, cur ? cur : "unknown/missing");
            free(cur);
        }
        config_free(g_cfg); db_close(g_db); free(db_path);
        return rc == 0 ? 0 : 1;
    }
    if (channel_mode && channel_check) {
        config_free(g_cfg); db_close(g_db);
        char *err = NULL;
        int rc = channel_runner_check(db_path, channel_mode, &err);
        if (rc == 0) {
            sqlite3 *cdb = db_open(db_path);
            int trc = cdb ? channel_mark_validated(cdb, channel_mode) : -1;
            if (cdb) db_close(cdb);
            if (trc == 0)
                printf("channel '%s': check passed, draft/broken -> validated\n", channel_mode);
            else
                fprintf(stderr, "channel '%s': check passed but the DB transition failed "
                                "(was it in 'draft' or 'broken'?)\n", channel_mode);
        } else {
            fprintf(stderr, "channel '%s': check FAILED: %s\n", channel_mode, err ? err : "?");
        }
        free(err);
        free(db_path);
        return rc == 0 ? 0 : 1;
    }
    if (channel_mode && channel_harness_scenario) {
        config_free(g_cfg); db_close(g_db);
        int rc = channel_harness_run(db_path, channel_mode, channel_harness_scenario);
        free(db_path);
        return rc;
    }

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
        LOG_WARN_("startup recovery failed");

    if (daemon_mode) return run_daemon(db_path);
    return run_cli(db_path, prompt, session_id, new_session, host_mode, auto_approve);
}
