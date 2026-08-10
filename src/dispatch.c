#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "dispatch.h"

#include "advance.h"
#include "agent_config.h"
#include "buf.h"
#include "agent_setup.h"
#include "approval.h"
#include "channel.h"
#include "channel_api.h"
#include "child.h"
#include "cli.h"
#include "config.h"
#include "config_registry.h"
#include "context.h"
#include "db.h"
#include "db_response.h"
#include "extension_manifest.h"
#include "hook_dispatch.h"
#include "llm_worker.h"
#include "log.h"
#include "loop.h"
#include "proc.h"
#include "resolve.h"
#include "run_tool.h"
#include "sandbox.h"
#include "secret.h"
#include "secret_capture.h"
#include "secret_interp.h"
#include "secret_scan.h"
#include "secret_store.h"
#include "skills.h"
#include "tool_args.h"
#include "tool_file.h"
#include "tool_js.h"
#include "tool_policy.h"
#include "tool_request_config.h"
#include "tool_thread.h"
#include "tools.h"
#include "types.h"
#include "unicode_normalize.h"
#include "util.h"
#include "validate.h"
#include "wake.h"
#include "web.h"

/* One slot per potential child: a session only lands here when the child
 * ceiling (or a gate that frees on a child completing) blocked it, so it can
 * never need more entries than there are children to wait on. */
#define STALLED_MAX CHILD_MAX

/* Sessions parked on a capacity ceiling, re-advanced when capacity frees.
 * Two feeders: tool dispatch that hit CHILD_MAX (dispatch_tool returned -1 —
 * the session sits in tool_running with pending, un-forked tool_calls, and no
 * reap would ever re-advance it) and turn opens deferred by the
 * session_max_concurrent drain gate (advance_session returned NOOP+deferred —
 * the inbox rows stay queued). Draining on reap / worker / tool-thread
 * completion re-runs advance_session, which is idempotent: still over a
 * ceiling → the session just re-enters the list. Ephemeral scheduling hint,
 * never a source of truth; session_sweep_inbox and db_recover_stale_sessions
 * are the durable backstops (overflow here just means sweep-tick latency). */
static int64_t g_stalled[STALLED_MAX];
static int g_stalled_count;

void stalled_add(int64_t session_id) {
    for (int i = 0; i < g_stalled_count; i++)
        if (g_stalled[i] == session_id) return;   /* dedup */
    if (g_stalled_count < STALLED_MAX)
        g_stalled[g_stalled_count++] = session_id;
}

/* Forward declarations */
static void cron_script_post(ChildProc *c, const char *output, const char *hosts);

/* ── dispatch_llm_req ────────────────────────────────────────────── */

/* Dispatch LLM request via worker thread pool */

/* One-shot user notice for a deferred (not failed) turn — rate limit or cost
 * ceiling. No assistant entry: the leaf must stay the user entry so
 * session_sweep_inbox keeps retrying until the window clears. Daemon mode
 * posts to the session's channel; CLI prints and releases the prompt. */
static void notify_deferred(int64_t session_id, const char *text) {
    if (!proc_is_daemon()) {
        fprintf(stderr, "\n%s\n", text);
        if (session_id == proc_cli_session()) proc_set_cli_turn_active(0);
        return;
    }
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(proc_db(),
            "INSERT INTO channel_outbox(channel_name, session_id, payload)"
            " SELECT channel_name, id,"
            "        json_object('chat_id', COALESCE(chat_id,'0'), 'text', ?2)"
            "   FROM sessions WHERE id=?1 AND channel_name IS NOT NULL;",
            -1, &s, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(s, 1, session_id);
    sqlite3_bind_text(s, 2, text, -1, SQLITE_STATIC);
    sqlite3_step(s);
    sqlite3_finalize(s);
    if (sqlite3_changes(proc_db()) > 0 && proc_cfg() && proc_cfg()->db_path) {
        char *ch = db_scalar_text(proc_db(),
            "SELECT channel_name FROM sessions WHERE id=?1;", session_id);
        if (ch) { channel_outbox_wake(proc_cfg()->db_path, ch); free(ch); }
    }
}

/* Should a refused dispatch (rate limit, budget) tell the user their message
 * is queued? True when the leaf entry is an unanswered user message and no
 * deferral notice was queued since it arrived. Inbox consumption happens in
 * advance_session's claim, not here, so a consumed-count signal always reads
 * zero in daemon mode — and the sweeper re-advances parked sessions every
 * tick, so an unconditional notify would spam the channel. */
static int should_notify_deferred(int64_t session_id) {
    if (!proc_is_daemon()) return 1; /* CLI: stderr, no sweeper, no spam risk */
    return (int)db_scalar_i64(proc_db(),
        "SELECT EXISTS(SELECT 1 FROM sessions s"
        " JOIN entries e ON e.id=s.leaf_id AND e.session_id=s.id"
        " WHERE s.id=?1 AND e.role=1 AND s.channel_name IS NOT NULL"
        " AND NOT EXISTS(SELECT 1 FROM channel_outbox o"
        "   WHERE o.session_id=s.id AND o.created_at>=e.created_at"
        "     AND (json_extract(o.payload,'$.text') LIKE 'rate limited:%'"
        "       OR json_extract(o.payload,'$.text') LIKE 'budget limit:%'"
        "       OR json_extract(o.payload,'$.text') LIKE 'disk low:%')))",
        session_id, 0);
}

int dispatch_llm_req(int64_t session_id, const char *agent_name, int iteration) {
    if (child_has_session(session_id)) return -1;

    /* Turn start: move queued inbox into entries (backstop — the daemon path
     * normally consumes inside advance_session's claim). */
    if (iteration == 0)
        inbox_consume_into_entries(proc_db(), session_id, 100);

    if (!llm_worker_alive()) return -1;

    int max_iter = session_max_iter(session_id);
    if (iteration >= max_iter) {
        /* The advance_session check normally catches this; this is a fallback.
         * Simpler than advance.c's rich message, but it still carries the
         * norm: the limit, cut-mid-flight, and that continuing is legal. */
        char cut[256];
        snprintf(cut, sizeof(cut),
                 "error: iteration limit reached (%d per turn) — work was cut"
                 " mid-flight, not finished. Continuing where you left off"
                 " next turn is legal and expected.", max_iter);
        Message msg = {.role = ROLE_ASSISTANT,
                       .content = cut,
                       .stop_reason = STOP_REASON_ERROR};
        entry_append_with_iteration(proc_db(), session_id, &msg, 0);
        session_set_state(proc_db(), session_id, "idle");
        if (!proc_is_daemon()) {
            fprintf(stderr, "\n%s\n", cut);
            proc_set_cli_turn_active(0);
        }
        return -1;
    }

    /* Rate limit check */
    if (!rate_limit_check(proc_db(), proc_cfg()->token_rate_limit)) {
        LOG_WARN_("token_rate_limit hit, session %lld rate_limited",
                  (long long)session_id);
        session_set_state(proc_db(), session_id, "rate_limited");
        if (should_notify_deferred(session_id))
            notify_deferred(session_id,
                "rate limited: hourly token cap reached — your message is"
                " queued and will be answered when the limit clears");
        return -1;
    }

    /* Daily cost ceiling — rolling 24h. Unpriced models record cost 0, so the
     * token cap above stays the always-available brake. */
    if (proc_cfg()->daily_cost_limit_nano > 0) {
        int64_t spent = db_cost_last_24h(proc_db());
        if (spent >= proc_cfg()->daily_cost_limit_nano) {
            LOG_WARN_("daily_cost_limit hit spent_nano=%lld limit_nano=%lld,"
                      " session %lld rate_limited",
                      (long long)spent, (long long)proc_cfg()->daily_cost_limit_nano,
                      (long long)session_id);
            session_set_state(proc_db(), session_id, "rate_limited");
            if (should_notify_deferred(session_id))
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
        long free_mb = db_free_mb(proc_db());
        if (free_mb >= 0 && free_mb < disk_floor_mb) {
            LOG_WARN_("disk_low free_mb=%ld floor_mb=%d deferring llm dispatch",
                      free_mb, disk_floor_mb);
            session_set_state(proc_db(), session_id, "idle");
            if (should_notify_deferred(session_id))
                notify_deferred(session_id,
                    "disk low: free space is under the safety floor — your"
                    " message is queued and will be answered once space frees"
                    " up; the operator may need to clear disk");
            return -1;
        }
    }

    /* preAdvance hooks: run main-thread after the max-iter and rate-limit
     * gates (never fire for an unsent request); their commands cross to the
     * worker's payload build as DB state (hook_directives / entries.data). A
     * worker-submit failure below leaves directives behind for the retried
     * request — acceptable, llm_req clears them on every exit. */
    if (proc_tool_setup()) {
        char *cmds = hook_dispatch_pre_advance(&proc_tool_setup()->ext_ctx, proc_db(), session_id);
        if (cmds) {
            hook_apply_pre_advance(proc_db(), session_id, cmds);
            free(cmds);
        }
    }

    session_set_state(proc_db(), session_id, "llm_running");
    int rc = llm_worker_submit(proc_db(), session_id, agent_name, iteration == 0 ? 1 : 0);
    if (rc < 0) {
        /* No worker accepted the job — don't strand the session in llm_running
         * (no completion will ever fire to move it off). Revert to idle. */
        session_set_state(proc_db(), session_id, "idle");
    }
    return rc;
}

/* Re-advance sessions stalled on the child ceiling. Called after a tool slot
 * frees (reap). Snapshot-and-clear first: run_advance may re-stall a session
 * if the ceiling is still hit, re-populating the set for the next reap. */
void stalled_drain(void) {
    if (g_stalled_count == 0) return;
    int64_t snap[STALLED_MAX];
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
    /* cron_set only parks when the job would run as another agent. */
    { "cron_set",          0, 0, NULL_PARK },
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

/* Append an inline error tool-result and mark the call done. `detail` is the
 * status detail column (may be NULL). Always returns 1 (handled inline). */
static int tool_inline_error(int64_t session_id, PendingToolCall *tc,
                             const char *msg, const char *detail) {
    char *err = strdup(msg);
    ToolResult tr = {.tool_call_id = tc->call_id, .content = err};
    Message m = {.role = ROLE_TOOL, .tool_result = &tr,
                 .tool_name = tc->name, .is_error = 1};
    entry_append_with_iteration(proc_db(), session_id, &m, tc->iteration_id);
    db_tool_call_set_status(proc_db(), session_id, tc->call_id, "done", detail);
    free(err);
    return 1;
}

/* Same, for a non-error inline result (the approval background notice): the
 * call is answered and done, but nothing went wrong. Always returns 1. */
static int tool_inline_notice(int64_t session_id, PendingToolCall *tc,
                              const char *msg, const char *detail) {
    char *note = strdup(msg);
    ToolResult tr = {.tool_call_id = tc->call_id, .content = note};
    Message m = {.role = ROLE_TOOL, .tool_result = &tr,
                 .tool_name = tc->name, .is_error = 0};
    entry_append_with_iteration(proc_db(), session_id, &m, tc->iteration_id);
    db_tool_call_set_status(proc_db(), session_id, tc->call_id, "done", detail);
    free(note);
    return 1;
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

/* First {{SECRET:name}} placeholder in args naming no loaded secret, or NULL.
 * Caller frees. A typo'd name used to interpolate to nothing and sail on —
 * an auth failure attributed to everything except the actual cause. */
static char *unknown_secret_name(const char *args,
                                 const ShellSecret *secrets, size_t count) {
    size_t n = 0;
    char **names = secret_placeholder_names(args, &n);
    if (!names) return NULL;
    char *miss = NULL;
    for (size_t i = 0; i < n && !miss; i++) {
        int loaded = 0;
        for (size_t j = 0; j < count; j++)
            if (strcmp(names[i], secrets[j].name) == 0) { loaded = 1; break; }
        if (!loaded) miss = strdup(names[i]);
    }
    secret_names_free(names, n);
    return miss;
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
 *    to the union of those secrets' bound hosts, unconditionally — no
 *    approval waives it. Fail-closed: an unbound secret yields an empty
 *    allow list (deny-all); the way past is a request_config
 *    secret_bindings document (D17), never a wider run. */
typedef struct {
    char **deny; size_t deny_n;          /* owned labels for req.deny_rules */
    char **bound; size_t bound_n;        /* owned union of bound hosts */
    char **decl; size_t decl_n;          /* owned declared hosts (promoted tool) */
    int narrowed;
    char *note;                          /* owned req.egress_note when narrowed */
    const char **ext; size_t ext_n;      /* owned array, borrowed strings */
    const char **hosts; size_t hosts_n;  /* what the req should carry */
} CallEgress;

static void call_egress_build(CallEgress *ce, const char *tool_name,
                              const char *args,
                              const char *sens_exception,
                              char **base_hosts, size_t base_n,
                              const ShellSecret *secrets, size_t secret_count) {
    memset(ce, 0, sizeof(*ce));
    int n = 0;
    char **all = db_sensitive_hosts(proc_db(), &n);
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
    ce->decl = db_tool_declared_hosts(proc_db(), tool_name, &dn);
    ce->decl_n = (size_t)dn;

    const char **hosts = ce->decl ? (const char **)ce->decl
                                  : (const char **)base_hosts;
    size_t hosts_n = ce->decl ? ce->decl_n : base_n;
    if (args) {
        size_t un = 0;
        char **names = used_secret_names(args, secrets, secret_count, &un);
        if (names) {
            /* Compose the deny-summary advice alongside the union: the child
             * appends it when the proxy denies, so the model learns the deny
             * was a missing *binding*, not a missing host grant. */
            Buf nb = {0};
            buf_append_str(&nb, "this call ran narrowed to its secrets' "
                                "bound hosts (");
            char **u = NULL; size_t uc = 0, ucap = 0;
            for (size_t i = 0; i < un; i++) {
                int bn = 0;
                char **bh = db_secret_hosts(proc_db(), names[i], &bn);
                buf_appendf(&nb, "%s%s ->", i ? "; " : "", names[i]);
                if (bn == 0) buf_append_str(&nb, " none");
                for (int j = 0; j < bn; j++) {
                    buf_appendf(&nb, " %s", bh[j]);
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
            buf_append_str(&nb, ") — a blocked host means that secret lacks "
                                "a binding for it; request the pair via "
                                "request_config (secret_bindings), then "
                                "re-issue");
            ce->note = buf_take(&nb);   /* NULL on OOM: generic advice then */
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
    free(ce->note);
    free((void *)ce->ext);
    memset(ce, 0, sizeof(*ce));
}

/* §4+§8 dispatch gate, split from dispatch_tool_inner (2026-07-19).
 * Capability ceiling → policy → gating hook → sensitivity scan →
 * unbound-secret deny → approval gate, in fixed restrict-only order. Returns
 * 0 = proceed (an approved once-only sensitivity park is reported via
 * *sens_once and sens_host for the per-call egress exception), 1 = handled
 * (error result written), 2 = parked awaiting approval. */
static int dispatch_gate(int64_t session_id, const char *agent_name,
                         PendingToolCall *tc, ToolEntry *te,
                         const ShellSecret *secrets, size_t secret_count,
                         char *sens_host, size_t sens_host_cap,
                         int *sens_once) {
    int sens_hit = 0;
    *sens_once = 0;
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
        if (!grants_contains(proc_db(), agent_name, "tool", tc->name)) {
            char err[160];
            snprintf(err, sizeof(err),
                     "error: %s not granted — request it with request_config",
                     tc->name);
            ToolResult tr = {.tool_call_id = tc->call_id, .content = err};
            Message msg = {.role = ROLE_TOOL, .tool_result = &tr,
                           .tool_name = tc->name, .is_error = 1};
            entry_append_with_iteration(proc_db(), session_id, &msg, tc->iteration_id);
            db_tool_call_set_status(proc_db(), session_id, tc->call_id, "done", "not_granted");
            return 1;
        }
        /* Session scope: a spawn-frozen tool_filter narrows the grant set for
         * this session only (grants ∩ filter). Checked after grants so the
         * filter can never widen authority, only shrink it. */
        if (!session_tool_allowed(proc_db(), session_id, tc->name)) {
            /* Teach the route-around, not just the wall: a filtered session is
             * a narrowed *spawn*, not a missing grant, so the fix lives with
             * whoever launched it. Without this the model can only infer the
             * filter's existence from the word "filter" (observed, CharlesDow
             * 2026-07-31). */
            char err[288];
            snprintf(err, sizeof(err),
                     "error: %s blocked by this session's tool filter"
                     " (this session was spawned with a narrowed toolset; the"
                     " spawner can pass tools:[...] within its own grants)",
                     tc->name);
            return tool_inline_error(session_id, tc, err, "tool_filter");
        }
        /* Malformed arguments fail closed at the gate: the model gets an
         * error tool-result and can retry. Nothing downstream (policy, hooks,
         * handlers, the child wire) ever sees invalid JSON, so no code path
         * can substitute an empty param set for unparseable args. */
        if (!tool_args_valid_object(proc_db(), tc->arguments)) {
            char err[160];
            snprintf(err, sizeof(err),
                     "error: %s: arguments are not a valid JSON object — "
                     "retry the call with well-formed JSON", tc->name);
            return tool_inline_error(session_id, tc, err, "bad_args");
        }
        ToolApprovalMode mode = agent_tool_mode(proc_db(), agent_name, tc->name);
        HookGate gate = (mode == TOOL_MODE_SILENT) ? HOOK_GATE_ALLOW : HOOK_GATE_ASK;

        /* Per-argument policy pre-filter (restrict-only, before hooks) */
        if (te->policy_json) {
            PolicyEffect pe = policy_eval(proc_db(), tc->arguments, te->policy_json);
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
                entry_append_with_iteration(proc_db(), session_id, &msg, tc->iteration_id);
                db_tool_call_set_status(proc_db(), session_id, tc->call_id, "done", "policy:deny");
                return 1;
            }
            if (pe == POLICY_ASK && gate < HOOK_GATE_ASK)
                gate = HOOK_GATE_ASK;
        }

        if (proc_tool_setup()) {
            char *reason = NULL;
            HookGate h = hook_dispatch_gate_tool_call(&proc_tool_setup()->ext_ctx, proc_db(),
                                                      tc->name, tc->arguments, &reason);
            if (h > gate) gate = h;  /* restrict-only: most restrictive wins */
            if (gate == HOOK_GATE_DENY) {
                char err[256];
                snprintf(err, sizeof(err), "error: %s blocked by hook%s%s", tc->name,
                         reason ? ": " : "", reason ? reason : "");
                ToolResult tr = {.tool_call_id = tc->call_id, .content = err};
                Message msg = {.role = ROLE_TOOL, .tool_result = &tr,
                               .tool_name = tc->name, .is_error = 1};
                entry_append_with_iteration(proc_db(), session_id, &msg, tc->iteration_id);
                db_tool_call_set_status(proc_db(), session_id, tc->call_id, "done", "hook:deny");
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
            char **sens = db_sensitive_hosts(proc_db(), &sn);
            if (sens) {
                sens_hit = host_in_text(sens, (size_t)sn, tc->arguments,
                                        sens_host, sens_host_cap);
                /* Second chance on the *extracted, decoded* url host: a
                 * percent- or invisible-Unicode-encoded host tokenizes wrong
                 * in the raw-args scan above, which would skip the ASK
                 * escalation (r2 F11). */
                if (!sens_hit) {
                    char uh[254];
                    if (web_args_url_host(proc_db(), tc->arguments, uh, sizeof(uh))) {
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
        /* Fail-closed credential rule (specs/trust.md rule 2): a loaded
         * secret may only be submitted to its bound hosts. No park, no
         * approval class — the gate denies with attribution and the agent
         * requests the binding via request_config (secret_bindings), the
         * same shape as a proxy host denial (D17). An unbound secret denies
         * on every tier: the call would run narrowed to deny-all, so nothing
         * useful can happen with the credential anyway, and the secret never
         * enters a child that couldn't use it. The web tier's static url
         * host makes the uncovered-host case precise; shell/js runtime
         * targets are enforced by the narrowing in call_egress_build and
         * attributed by the proxy-deny note. */
        {
            size_t un = 0;
            char **names = used_secret_names(tc->arguments, secrets, secret_count, &un);
            if (names) {
                char url_host[254] = "";
                int have_url = (te->recipe.tier == SBX_WEB) &&
                    web_args_url_host(proc_db(), tc->arguments, url_host,
                                      sizeof(url_host));
                char err[1024] = "";
                for (size_t i = 0; i < un && !err[0]; i++) {
                    int bn = 0;
                    char **bh = db_secret_hosts(proc_db(), names[i], &bn);
                    if (!bh) {
                        snprintf(err, sizeof(err),
                                 "error: secret %.63s has no host binding — request "
                                 "one with request_config (secret_bindings: {\"%.63s\": "
                                 "[\"%.253s\"]}), then re-issue this call",
                                 names[i], names[i],
                                 have_url ? url_host : "<host it needs>");
                    } else {
                        if (have_url && !host_covered(bh, (size_t)bn, url_host))
                            snprintf(err, sizeof(err),
                                     "error: secret %.63s is not bound to %.253s — "
                                     "request the binding with request_config "
                                     "(secret_bindings: {\"%.63s\": [\"%.253s\"]}), "
                                     "then re-issue this call",
                                     names[i], url_host, names[i], url_host);
                        for (int j = 0; j < bn; j++) free(bh[j]);
                        free(bh);
                    }
                }
                secret_names_free(names, un);
                if (err[0])
                    return tool_inline_error(session_id, tc, err, "secret:unbound");
            }
        }
        if (gate == HOOK_GATE_ASK) {
            const char *ask_action = sens_hit ? "sensitive" : tc->name;
            Approval *ap = approval_get_for_tool_call(proc_db(), session_id, tc->call_id);
            int approved = ap && ap->state && strcmp(ap->state, "approved") == 0;
            int denied   = ap && ap->state && strcmp(ap->state, "denied") == 0;
            int pending  = ap && ap->state && strcmp(ap->state, "pending") == 0;
            if (denied) {
                char err[128];
                snprintf(err, sizeof(err), "error: %s denied by user", tc->name);
                ToolResult tr = {.tool_call_id = tc->call_id, .content = err};
                Message msg = {.role = ROLE_TOOL, .tool_result = &tr,
                               .tool_name = tc->name, .is_error = 1};
                entry_append_with_iteration(proc_db(), session_id, &msg, tc->iteration_id);
                db_tool_call_set_status(proc_db(), session_id, tc->call_id, "done", "denied");
                approval_free(ap);
                return 1;
            }
            /* Ticket transfer: a post-window YES lands on a frozen call that
             * was already answered "still pending" — its approval row stays
             * approved and unconsumed. It covers the next call asking for the
             * same capability, once, so the model's instructed re-issue runs
             * instead of re-asking the human. Sensitivity parks never
             * transfer (per-call, trust.md rule 1). */
            int via_ticket = 0;
            if (!approved && !pending && !sens_hit) {
                int64_t tk = approval_take_ticket(proc_db(), session_id, ask_action,
                                                  tc->name, tc->arguments);
                if (tk > 0) {
                    LOG_INFO_("approval ticket consumed id=%lld tool=%s call=%s",
                              (long long)tk, tc->name, tc->call_id);
                    approved = 1;
                    via_ticket = 1;
                }
            }
            if (!approved) {
                /* none → create + park; pending → re-park idempotently.
                 * A sensitivity hit is recorded as action='sensitive' so the
                 * resolve path can refuse to mint anything standing from it. */
                if (!pending) {
                    /* Dedupe: this capability is already parked on another
                     * call — a second row storms the approver, and every
                     * duplicate park costs a full LLM iteration to unpark. */
                    int64_t dup = approval_find_pending_match(proc_db(), session_id,
                                      ask_action, tc->name, tc->arguments);
                    if (dup > 0) {
                        char err[256];
                        snprintf(err, sizeof(err),
                                 "error: %s needs approval and a matching request is "
                                 "already parked as approval #%lld — do not re-issue "
                                 "this call or a variant; you'll be notified when it's "
                                 "decided", tc->name, (long long)dup);
                        approval_free(ap);
                        return tool_inline_error(session_id, tc, err, "approval:dup");
                    }
                    char idlist[96];
                    int npend = approval_pending_ids(proc_db(), session_id,
                                                     idlist, sizeof(idlist));
                    int cap = config_get_int(proc_db(), "approval_max_pending");
                    if (cap <= 0) cap = config_default_int("approval_max_pending");
                    if (npend >= cap) {
                        char err[256];
                        snprintf(err, sizeof(err),
                                 "error: %d approvals already pending (%s) — no more "
                                 "will be requested; wait for those decisions or work "
                                 "on something that doesn't need approval",
                                 npend, idlist);
                        approval_free(ap);
                        return tool_inline_error(session_id, tc, err, "approval:cap");
                    }
                    approval_create(proc_db(), session_id, tc->call_id, tc->name,
                                    ask_action, tc->arguments, "rerun");
                }
                session_set_state(proc_db(), session_id, "awaiting_approval");
                approval_free(ap);
                handle_approval_park(session_id, tc->call_id);
                /* Zero block window (ambient route, or approval_block_sec=0):
                 * the operator has been prompted, so answer the call inline
                 * with the background notice instead of freezing the turn.
                 * The late decision still arrives as an inbox follow-up.
                 * Daemon only — in CLI mode handle_approval_park may have
                 * *deferred* an auto-decision that the loop applies against a
                 * still-parked call, and unparking here would strand it. */
                if (proc_is_daemon() &&
                    approval_block_seconds_for_session(proc_db(), session_id) == 0) {
                    Approval *bg = approval_get_for_tool_call(proc_db(), session_id,
                                                             tc->call_id);
                    if (bg && bg->state && strcmp(bg->state, "pending") == 0) {
                        char note[256];
                        approval_background_notice(bg->id, note, sizeof(note));
                        approval_free(bg);
                        session_set_state(proc_db(), session_id, "tool_running");
                        return tool_inline_notice(session_id, tc, note,
                                                  "approval:background");
                    }
                    approval_free(bg);
                }
                return 2; /* parked */
            }
            /* approved → consume (single-use) and fall through to execute the
             * frozen call. An ALWAYS decision flips the tool mode to silent so
             * it never reaches the gate again; only a "once" approval lands
             * here, and consuming it stops a replayed tool_call_id from
             * re-using the same grant. A consumed sensitivity approval opens a
             * per-call exception for the matched host (network tiers below).
             * A ticket was already consumed inside approval_take_ticket. */
            *sens_once = sens_hit;
            if (!via_ticket) approval_consume(proc_db(), ap->id);
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
     * proc_tool_setup() is a single shared instance, so the session_id captured
     * at agent_setup_init time is stale (0 in CLI) — the dispatching session
     * varies per call (root or any sub-agent). Keyed on ctx *pointer
     * identity*, not on an enumerated tool-name list: launch_agent and
     * check_session share launch_ctx.
     * Any future tool registered against these same shared ctx structs
     * is covered automatically; a tool needing its own fresh ctx should
     * get its own struct, not reuse one of these without adding it here. */
    if (te->user_data == &proc_tool_setup()->launch_ctx) {
        AgentLaunchCtx *lc = (AgentLaunchCtx *)te->user_data;
        lc->session_id = session_id;
        lc->current_tool_call_id = tc->call_id;
    } else if (te->user_data == &proc_tool_setup()->req_cfg_ctx) {
        RequestConfigCtx *rctx = (RequestConfigCtx *)te->user_data;
        rctx->session_id = session_id;
        rctx->current_tool_call_id = tc->call_id;
    } else if (te->user_data == &proc_tool_setup()->bootstrap_ctx) {
        ToolBootstrapCtx *bctx = (ToolBootstrapCtx *)te->user_data;
        bctx->session_id = session_id;
        bctx->current_tool_call_id = tc->call_id;
    } else if (te->user_data == &proc_tool_setup()->ext_tool_ctx) {
        ToolExtensionCtx *ectx = (ToolExtensionCtx *)te->user_data;
        ectx->session_id = session_id;
        ectx->current_tool_call_id = tc->call_id;
    } else if (te->user_data == &proc_tool_setup()->cron_ctx) {
        ToolCronCtx *kctx = (ToolCronCtx *)te->user_data;
        kctx->session_id = session_id;
        kctx->current_tool_call_id = tc->call_id;
        /* Job ownership follows the advancing agent, not setup's init agent. */
        snprintf(kctx->agent_name, sizeof(kctx->agent_name), "%s",
                 agent_name ? agent_name : "");
    } else if (te->user_data == &proc_tool_setup()->chan_send_ctx) {
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
            handle_approval_park(session_id, tc->call_id);
            return 2; /* parked, don't advance */
        }
        result = strdup("error: tool returned null");
    }

    /* Explicit capture first (needs the RAW result), then postprocess:
     * deinterpolate + secret scan/redact (scan runs even with no secrets
     * loaded — inline js_eval output can carry leaked credentials). */
    { char *cap = secret_capture_apply(proc_db(), tc->arguments, result);
      if (cap) { free(result); result = cap; } }
    { char *pp = tool_result_postprocess(result, secrets, secret_count);
      if (pp) { free(result); result = pp; } }

    /* afterToolCall hooks: chained result replacement, after the secret
     * scanner (security boundary stays first), before the entry write so
     * the replacement is what the context sees. The replacement comes back
     * already marker-sanitized (hook_dispatch owns that invariant). */
    char *hook_annotate = NULL;
    if (proc_tool_setup()) {
        char *rep = hook_dispatch_observe_tool_call(&proc_tool_setup()->ext_ctx,
                        proc_db(), tc->name, tc->arguments, result, &hook_annotate);
        if (rep) { free(result); result = rep; }
    }

    /* CLI progress */
    if (!proc_is_daemon()) {
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
    int64_t rid = entry_append_with_iteration(proc_db(), session_id, &msg, tc->iteration_id);
    db_tool_call_complete_with_result(proc_db(), tc->entry_id, tc->call_id, rid);
    if (hook_annotate) {
        if (rid > 0) hook_entry_data_patch(proc_db(), rid, hook_annotate);
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
    char *root = proc_db() ? config_get(proc_db(), "tmp_root") : NULL;
    const char *r = scratch_dir_ensure(agent_name, root, out, cap);
    free(root);
    return r;
}

/* Serialize one SBX_JS --run-tool request. Everything that makes it a JS-tier
 * request — the agent's sandbox profile, its grant mounts, the read-only
 * extension store, the proxy egress snapshot — is the same whether the caller
 * is a model tool call or a scheduled cron script; only the params and the
 * egress key differ, so those come in as arguments. `params` is consumed
 * (wiped and freed) either way. Returns the blob (caller wipes + frees) with
 * *out_len set, or NULL if it exceeds the wire cap. */
static char *js_request_serialize(JsEvalCtx *jc, const char *agent_name,
                                  const char *egress_tool, const char *egress_args,
                                  const char *sens_host,
                                  const ShellSecret *secrets, size_t secret_count,
                                  ToolWireArg *params, size_t param_n,
                                  size_t *out_len) {
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

    RunToolReq req;
    run_tool_req_init(&req, RUNTOOL_TIER_JS, "js_eval",
                      &jc->sb, jc->workspace, jc->cwd_path, jc->db_path);
    char scratch[PATH_MAX];
    req.tmp_dir = dispatch_scratch_dir(agent_name, scratch, sizeof(scratch));
    req.params = params;
    req.param_count = param_n;
    req.read_paths = read_paths;  /* transient override: + extension store */
    req.read_count = rc_count;
    req.agent_dir = agent_dir;
    CallEgress se;
    call_egress_build(&se, egress_tool, egress_args, sens_host,
                      jc->allowed_hosts, jc->allowed_hosts_count,
                      secrets, secret_count);
    req.host_rules = se.hosts;
    req.host_count = se.hosts_n;
    req.deny_rules = (const char **)se.deny;
    req.deny_count = se.deny_n;
    req.egress_note = se.note;
    req.timeout = 120;
    char *blob = run_tool_serialize_request(&req, out_len);
    call_egress_free(&se);
    free(read_paths);
    tool_wire_args_wipe_free(params, param_n);
    return blob;
}

static int dispatch_tool_inner(int64_t session_id, const char *agent_name,
                               PendingToolCall *tc,
                               const ShellSecret *secrets, size_t secret_count) {
    if (child_count() >= CHILD_MAX) return -1;

    ToolEntry *te = proc_tool_setup() ? tools_lookup(&proc_tool_setup()->reg, tc->name) : NULL;
    if (!te && proc_tool_setup()) {
        /* Registry is a cache of the extension-tool query; a miss may just
         * mean an extension was promoted/attached after startup, or that this
         * is a non-default agent whose tools were never materialized. Refresh
         * for the *advancing* agent and retry. Safe unlocked: the registry is
         * only touched on the event-loop thread. On TOOLS_MAX overflow the
         * register fails, the second lookup misses, and we fall through to the
         * unknown-tool error instead of corrupting the registry. */
        tools_load_extension_tools(&proc_tool_setup()->reg, proc_db(), agent_name,
                                   &proc_tool_setup()->js_eval_ctx);
        te = tools_lookup(&proc_tool_setup()->reg, tc->name);
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
        entry_append_with_iteration(proc_db(), session_id, &msg, tc->iteration_id);
        db_tool_call_set_status(proc_db(), session_id, tc->call_id, "done", NULL);
        return 1; /* Signal: handled inline, check for more */
    }

    /* Dispatch gate (capability → policy → hooks → sensitivity/secret →
     * approval). sens_host survives the gate: an approved once-only
     * sensitivity park grants that host to THIS call via a per-call
     * exception in the network-tier sections below. */
    char sens_host[254];
    int sens_once = 0;
    {
        int g = dispatch_gate(session_id, agent_name, tc, te,
                              secrets, secret_count,
                              sens_host, sizeof(sens_host),
                              &sens_once);
        if (g != 0) return g;
    }

    /* {{SECRET:name}} gate: every path below interpolates placeholders into
     * this call's values (inline needs_interp tools and all sandbox tiers).
     * A name that matches no loaded secret is a hard error naming the typo —
     * never an enumeration of what does exist (see search_config
     * secret_bindings for that, through the sanctioned surface). */
    if ((tool_needs_interpolation(tc->name) ||
         te->recipe.vehicle == EXEC_SANDBOX) &&
        tc->arguments && strstr(tc->arguments, "{{SECRET:")) {
        char *miss = unknown_secret_name(tc->arguments, secrets, secret_count);
        if (miss) {
            char err[256];
            snprintf(err, sizeof(err),
                     "error: {{SECRET:%s}} names no known secret — check the "
                     "spelling; search_config's secret_bindings section lists "
                     "the known names", miss);
            free(miss);
            return tool_inline_error(session_id, tc, err, "unknown_secret");
        }
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
            if (!proc_is_daemon())
                cli_print_tool_call(tc->name, tc->arguments);
            session_set_state(proc_db(), session_id, "tool_running");

            /* Host mode (sandbox=0) rides the SAME broker path: the child sets
             * up no namespace and run_file_tier runs the handler in-process,
             * exactly as the (now-deleted) generic fork path did. */
            /* Decompose arguments HERE (trusted parent, SQLite JSON1) — the
             * child receives pre-extracted params and parses no JSON. */
            ToolWireArg *params = NULL;
            size_t param_n = 0;
            if (tool_args_extract(proc_db(), tc->name, te->parameters_json,
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
            agent_dir_resolve(fctx->workspace, proc_cfg() ? proc_cfg()->db_path : NULL,
                              agents_dir_buf, sizeof(agents_dir_buf));
            char **skill_dirs = skills_dirs_resolve(proc_db(), agents_dir_buf,
                                                    agent_name, &skd_n);
            char store_dir[PATH_MAX] = {0};
            int have_store = 0;
            if (proc_cfg() && proc_cfg()->db_path && proc_cfg()->db_path[0]) {
                char tmp[PATH_MAX - 16];
                snprintf(tmp, sizeof(tmp), "%s", proc_cfg()->db_path);
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
                         tc->iteration_id, tc->entry_id, blob, blob_len, 120);
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
            if (!proc_is_daemon())
                cli_print_tool_call(tc->name, tc->arguments);
            session_set_state(proc_db(), session_id, "tool_running");

            char *command = tool_args_str(proc_db(), tc->arguments, "command");
            if (!command)
                return tool_inline_error(session_id, tc,
                    "error: shell_exec requires a 'command' string argument", NULL);
            int cmd_timeout = tool_args_int(proc_db(), tc->arguments, "timeout", sc->timeout);
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
            call_egress_build(&se, tc->name, tc->arguments,
                              sens_once ? sens_host : NULL,
                              sc->allowed_hosts, sc->allowed_host_count,
                              secrets, secret_count);
            req.host_rules = se.hosts;
            req.host_count = se.hosts_n;
            req.deny_rules = (const char **)se.deny;
            req.deny_count = se.deny_n;
            req.egress_note = se.note;
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
                         tc->iteration_id, tc->entry_id, blob, blob_len,
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
        if (!proc_is_daemon())
            cli_print_tool_call(tc->name, tc->arguments);
        session_set_state(proc_db(), session_id, "tool_running");

        /* Decompose arguments in the parent, then interpolate {{SECRET:X}}
         * per extracted value (a URL may carry a token). */
        ToolWireArg *params = NULL;
        size_t param_n = 0;
        if (tool_args_extract(proc_db(), tc->name, te->parameters_json,
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
        call_egress_build(&se, tc->name, tc->arguments,
                          sens_once ? sens_host : NULL,
                          wc->allowed_hosts, wc->allowed_host_count,
                          secrets, secret_count);
        req.host_rules = se.hosts;
        req.host_count = se.hosts_n;
        req.deny_rules = (const char **)se.deny;
        req.deny_count = se.deny_n;
        req.egress_note = se.note;
        req.timeout = 60;
        char *blob = run_tool_serialize_request(&req, &blob_len);
        call_egress_free(&se);
        tool_wire_args_wipe_free(params, param_n);
        if (!blob)
            return tool_inline_error(session_id, tc,
                "error: web request exceeds 32KB cap", NULL);
        int rc = spawn_run_tool_child(session_id, agent_name,
                     tc->call_id, tc->name, tc->arguments,
                     tc->iteration_id, tc->entry_id, blob, blob_len, 90);
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

        if (!proc_is_daemon())
            cli_print_tool_call(tc->name, tc->arguments);
        session_set_state(proc_db(), session_id, "tool_running");

        /* Params: js_eval ships its own schema-extracted code/filename/args;
         * an extension tool ships filename=<handler .qjs path>, the raw call
         * arguments as the opaque `args` blob QuickJS parses natively (no JSON
         * wrapping/escaping round-trip), and `config` — its extension's
         * settings, resolved here because the child has no DB. Then interpolate
         * {{SECRET:X}} per value (args or config may carry a token). */
        ToolWireArg *params = NULL;
        size_t param_n = 0;
        if (!handler_path) {
            if (tool_args_extract(proc_db(), tc->name, te->parameters_json,
                                  tc->arguments, &params, &param_n) != 0)
                return tool_inline_error(session_id, tc,
                    "error: invalid tool arguments", "bad_args");
        } else {
            char *ext_config = tool_extension_config_json(proc_db(), tc->name);
            params = calloc(3, sizeof(*params));
            if (params) {
                params[0].key = strdup("filename");
                params[0].kind = TOOL_ARG_TEXT;
                params[0].value = strdup(handler_path);
                params[1].key = strdup("args");
                params[1].kind = TOOL_ARG_JSON;
                params[1].value = strdup(tc->arguments && tc->arguments[0]
                                             ? tc->arguments : "{}");
                params[2].key = strdup("config");
                params[2].kind = TOOL_ARG_JSON;
                params[2].value = ext_config;   /* ownership moves to params */
                ext_config = NULL;
                param_n = 3;
            }
            free(ext_config);
            if (!params || !params[0].key || !params[0].value ||
                !params[1].key || !params[1].value ||
                !params[2].key || !params[2].value) {
                tool_wire_args_free(params, param_n);
                return tool_inline_error(session_id, tc,
                    "error: OOM building js request", NULL);
            }
        }
        wire_params_interpolate(params, param_n, secrets, secret_count);

        size_t blob_len = 0;
        char *blob = js_request_serialize(jc, agent_name, tc->name, tc->arguments,
                                          sens_once ? sens_host : NULL,
                                          secrets, secret_count,
                                          params, param_n, &blob_len);
        if (!blob)
            return tool_inline_error(session_id, tc,
                "error: js request exceeds 32KB cap", NULL);
        int rc = spawn_run_tool_child(session_id, agent_name,
                     tc->call_id, tc->name, tc->arguments,
                     tc->iteration_id, tc->entry_id, blob, blob_len, 150);
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
        if (!proc_is_daemon())
            cli_print_tool_call(tc->name, tc->arguments);
        session_set_state(proc_db(), session_id, "tool_running");

        ToolThreadJob job = {0};
        job.session_id = session_id;
        job.iteration_id = tc->iteration_id;
        job.entry_id = tc->entry_id;
        snprintf(job.tool_call_id, sizeof(job.tool_call_id), "%s", tc->call_id);
        snprintf(job.tool_name, sizeof(job.tool_name), "%s", tc->name);
        snprintf(job.agent_name, sizeof(job.agent_name), "%s", agent_name);
        job.args = strdup(tc->arguments ? tc->arguments : "{}");
        job.run = te->recipe.thread_run;
        /* No THREAD (DB) tool references {{SECRET}} — interpolation is a
         * sandbox-tier concern. The thread still postprocesses (DLP scan). */
        job.secrets = proc_tool_setup() ? proc_tool_setup()->secrets : NULL;
        job.secret_count = proc_tool_setup() ? proc_tool_setup()->secret_count : 0;

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

/* Per-call secret snapshot wrapper: env base (proc_tool_setup()->secrets, immutable
 * for the process lifetime) merged with a fresh read of the DB-backed secret
 * store, so a secret born mid-session (operator `secret set`, secret_create)
 * is visible on its very next dispatch. Scoped to this one call — never
 * shared with async work that outlives it (EXEC_THREAD re-snapshots itself
 * with its own db handle; see tool_thread.c). */
int dispatch_tool(int64_t session_id, const char *agent_name,
                  PendingToolCall *tc) {
    size_t snap_n = 0;
    ShellSecret *snap = secrets_snapshot(proc_db(),
        proc_tool_setup() ? proc_tool_setup()->secrets : NULL,
        proc_tool_setup() ? proc_tool_setup()->secret_count : 0, &snap_n);
    int rc = dispatch_tool_inner(session_id, agent_name, tc, snap, snap_n);
    secrets_snapshot_free(snap, snap_n);
    return rc;
}

/* ── cron script fires ──────────────────────────────────────────────
 * The daemon's half of cron.c's script dispatcher: resolve the file inside the
 * TARGET agent's workspace, build exactly the SBX_JS request a js_eval call
 * would, and fork it. Refreshing caps to the target agent first is what makes
 * "runs under that agent's sandbox_profile and grants" true — the shared setup
 * is rebound before every tool batch anyway, so borrowing it here between
 * turns costs nothing on the single event-loop thread. {{SECRET:name}} is
 * deliberately not interpolated: a cron script is a file on disk, not a
 * model-written argument, so there is no argument to substitute into. */
CronScriptRc cron_script_run(const CronScriptFire *f, char *err, size_t err_len) {
    if (!proc_tool_setup() || !proc_db()) {
        snprintf(err, err_len, "the daemon cannot run scripts right now");
        return CRON_SCRIPT_FAILED;
    }
    if (child_count() >= CHILD_MAX) return CRON_SCRIPT_BUSY;

    agent_setup_refresh_caps(proc_tool_setup(), proc_db(), f->agent_name);
    JsEvalCtx *jc = &proc_tool_setup()->js_eval_ctx;
    if (!jc->workspace || !jc->workspace[0]) {
        snprintf(err, err_len, "agent '%s' has no workspace", f->agent_name);
        return CRON_SCRIPT_FAILED;
    }
    /* Two legal shapes, re-validated at fire time (cron_set checked when the
     * job was written, but the job outlives that check): a workspace-relative
     * path — what an agent may name — or an absolute path inside the shared
     * extension store, which is what a promoted extension's scheduled script
     * is and which the JS child already mounts read-only. Nothing else. */
    char full[PATH_MAX];
    struct stat sb;
    int n;
    if (f->script[0] == '/') {
        n = snprintf(full, sizeof(full), "%s", f->script);
        if (n <= 0 || (size_t)n >= sizeof(full) ||
            !extension_path_in_store(proc_db(), full)) {
            snprintf(err, err_len, "script '%s' is outside the extension store",
                     f->script);
            return CRON_SCRIPT_FAILED;
        }
    } else {
        n = snprintf(full, sizeof(full), "%s/%s", jc->workspace, f->script);
        if (strstr(f->script, "..") || n <= 0 || (size_t)n >= sizeof(full)) {
            snprintf(err, err_len, "script '%s' is not a workspace-relative path",
                     f->script);
            return CRON_SCRIPT_FAILED;
        }
    }
    if (stat(full, &sb) != 0 || !S_ISREG(sb.st_mode)) {
        snprintf(err, err_len, "script '%s' is missing or not a file", full);
        return CRON_SCRIPT_FAILED;
    }

    ToolWireArg *params = calloc(1, sizeof(*params));
    if (params) {
        params[0].key = strdup("filename");
        params[0].kind = TOOL_ARG_TEXT;
        params[0].value = strdup(full);
    }
    if (!params || !params[0].key || !params[0].value) {
        tool_wire_args_free(params, params ? 1 : 0);
        snprintf(err, err_len, "out of memory building the script request");
        return CRON_SCRIPT_FAILED;
    }

    size_t blob_len = 0;
    char *blob = js_request_serialize(jc, f->agent_name, "js_eval", "{}",
                                      NULL, NULL, 0, params, 1, &blob_len);
    if (!blob) {
        snprintf(err, err_len, "the script request exceeds the 32KB wire cap");
        return CRON_SCRIPT_FAILED;
    }
    ChildProc *c = spawn_run_tool_blob(CHILD_CRON_SCRIPT, f->session_id,
                                       f->agent_name, blob, blob_len, 150);
    explicit_bzero(blob, blob_len);
    free(blob);
    if (!c) return CRON_SCRIPT_BUSY;
    snprintf(c->cron_job, sizeof(c->cron_job), "%s", f->job_name ? f->job_name : "");
    c->cron_prompt = f->prompt ? strdup(f->prompt) : NULL;
    LOG_INFO_("cron script fork job=%s agent=%s session=%lld",
              c->cron_job, f->agent_name, (long long)f->session_id);
    return CRON_SCRIPT_SPAWNED;
}

/* ── reap_children (state machine) ──────────────────────────────── */

/* ── child_sweep_deadlines: kill timed-out children ──────────── */

static char *child_output_finalize(ChildProc *c, int status, char **hosts_out);

void child_sweep_deadlines(void) {
    time_t now = time(NULL);
    for (int i = 0; i < child_count(); i++) {
        ChildProc *c = child_at(i);
        if (c->deadline == 0 || c->pid <= 0 || now < c->deadline)
            continue;
        kill(c->pid, SIGKILL);
        if (c->type == CHILD_TOOL_EXEC) {
            /* Mirror the broker's own timeout treatment: keep whatever
             * partial output already crossed the pipe, and name the tool —
             * "which call died, and what had it said" is the debugging
             * trail the model gets. child_output_finalize runs the full
             * sanitize pipeline (drain, unicode strip, capture, secret
             * scan) — partial output must not skip it. Status 0: the child
             * isn't reaped yet, and we don't want a synthesized crash line. */
            char *hosts = NULL;
            char *partial = child_output_finalize(c, 0, &hosts);
            size_t plen = partial ? strlen(partial) : 0;
            char *errbuf = malloc(plen + 128);
            if (errbuf) {
                int hl = snprintf(errbuf, 128,
                         "error: tool timed out (%ds; raise with the timeout"
                         " parameter)", c->timeout_sec > 0 ? c->timeout_sec : 120);
                if (plen > 0) {
                    errbuf[hl] = '\n';
                    memcpy(errbuf + hl + 1, partial, plen + 1);
                }
            }
            free(partial);
            ToolResult tr = {.tool_call_id = c->tool_call_id,
                             .content = errbuf ? errbuf : "error: tool timed out"};
            Message msg = {.role = ROLE_TOOL, .tool_result = &tr,
                           .tool_name = c->tool_name, .is_error = 1};
            entry_append_with_iteration(proc_db(), c->session_id, &msg, c->iteration_id);
            db_tool_call_complete_with_result(proc_db(), c->entry_id, c->tool_call_id, -1);
            free(errbuf);
        } else if (c->type == CHILD_CRON_SCRIPT) {
            if (c->result_pipe >= 0) { close(c->result_pipe); c->result_pipe = -1; }
            free(c->outbuf); c->outbuf = NULL; c->outbuf_len = 0;
            char errbuf[96];
            snprintf(errbuf, sizeof(errbuf), "error: cron script timed out (%ds)",
                     c->timeout_sec > 0 ? c->timeout_sec : 120);
            /* is_error=1 keeps the failure visible in-session and countable by
             * the job's auto-pause streak. */
            cron_script_post(c, errbuf, NULL);
        }
        c->deadline = -1; /* mark consumed so reap doesn't double-advance */
    }
}

/* Everything a reaped --run-tool child's output goes through before anybody
 * decides what to *do* with it: drain the rest of the pipe, synthesize an
 * error for a crash that said nothing, strip invisible Unicode from anything
 * that touched the network, then explicit capture + the secret scan. Shared by
 * the tool-result path and the cron script path — the security steps must not
 * fork into two versions. *hosts_out borrows the child's meta (NULL when the
 * run had no network exposure). Returns heap output the caller frees. */
static char *child_output_finalize(ChildProc *c, int status, char **hosts_out) {
    /* Crash detection: signal or nonzero exit → synthesize error */
    int crashed = WIFSIGNALED(status) ||
                  (WIFEXITED(status) && WEXITSTATUS(status) != 0);

    child_drain_pipe(c);
    if (c->result_pipe >= 0) { close(c->result_pipe); c->result_pipe = -1; }

    char *output;
    if (crashed && (!c->outbuf || c->outbuf_len == 0)) {
        char err[128];
        if (WIFSIGNALED(status))
            snprintf(err, sizeof(err), "error: tool killed by signal %d", WTERMSIG(status));
        else
            snprintf(err, sizeof(err), "error: tool exited %d", WEXITSTATUS(status));
        output = strdup(err);
    } else if (c->outbuf) {
        output = c->outbuf;
        c->outbuf = NULL;          /* ownership moves to the caller */
        c->outbuf_len = 0;
    } else {
        output = strdup("");
    }
    if (!output) output = strdup("error: OOM");
    size_t out_len = output ? strlen(output) : 0;

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
    }
    /* (Forged fence markers are neutralized for ALL results —
     * network-tagged or not — inside tool_result_postprocess.) */

    /* Explicit capture first (raw result), then postprocess:
     * deinterpolate + scan/redact. Fresh per-call snapshot (same
     * rationale as dispatch_tool) — a secret born mid-session must
     * be maskable in a reaped sandboxed child's output too. */
    if (c->tool_args) {
        char *cap = secret_capture_apply(proc_db(), c->tool_args, output);
        if (cap) { free(output); output = cap; }
    }
    { size_t snap_n = 0;
      ShellSecret *snap = secrets_snapshot(proc_db(),
          proc_tool_setup() ? proc_tool_setup()->secrets : NULL,
          proc_tool_setup() ? proc_tool_setup()->secret_count : 0, &snap_n);
      char *pp = tool_result_postprocess(output, snap, snap_n);
      secrets_snapshot_free(snap, snap_n);
      if (pp) { free(output); output = pp; } }

    *hosts_out = hosts;
    return output;
}

static char *cron_inbox_payload(const char *prompt, const char *out) {
    const char *head = prompt ? prompt : "";
    const char *sep = prompt ? "\n\n" : "";
    size_t n = strlen(head) + strlen(sep) + sizeof("script output:\n") + strlen(out);
    char *s = malloc(n);
    if (s) snprintf(s, n, "%s%sscript output:\n%s", head, sep, out);
    return s;
}

/* A finished cron script's output enters the session. Two doors, and which one
 * is legal depends on the clock: between turns the result is appended straight
 * to the branch as a cron_result entry (a boundary is exactly where an entry
 * may be born) and ordinary delivery carries it out; mid-turn that write would
 * break the mid-turn invariant, so it queues as an inbox row instead and
 * drains as an annotated user entry at the next boundary. A 'both' payload
 * always takes the inbox door — prompt and script output must arrive as one
 * user entry, which is what starts the turn that reads them. */
static void cron_script_post(ChildProc *c, const char *output, const char *hosts) {
    int64_t sid = c->session_id;
    int is_err = (strncmp(output, "error:", 6) == 0);
    char *clean = utf8_sanitize(output, strlen(output));
    const char *text = clean ? clean : output;

    LOG_INFO_("cron script done job=%s session=%lld is_err=%d",
              c->cron_job, (long long)sid, is_err);

    if (c->cron_prompt) {
        char *payload = cron_inbox_payload(c->cron_prompt, text);
        if (payload) {
            inbox_insert_scanned(proc_db(), sid, "cron", c->cron_job, payload);
            free(payload);
        }
        wake_session(sid);
        free(clean);
        return;
    }

    int idle = db_scalar_i64(proc_db(), "SELECT state='idle' FROM sessions WHERE id=?;",
                             sid, 0) == 1;
    if (!idle) {
        char *payload = cron_inbox_payload(NULL, text);
        if (payload) {
            inbox_insert(proc_db(), sid, "cron", c->cron_job, payload);
            free(payload);
        }
        wake_session(sid);
        free(clean);
        return;
    }

    int64_t rid = entry_append_cron_result(proc_db(), sid, c->cron_job, text, is_err);
    if (rid > 0 && hosts) db_entry_set_network_hosts(proc_db(), rid, hosts);
    free(clean);

    /* Ordinary delivery: the session's edges decide (specs/delivery.md) — a
     * chat-bound fire ships via its channel edge, a parented fire via its
     * standing parent edge. A plain session has no edges, keeps the entry and
     * says nothing — the model reads it on its next turn. include_channel
     * even on error: a failed script's output always reached the chat. */
    advance_deliver_boundary(proc_db(), sid, is_err, 1);
    deliver_response(sid);   /* daemon FIFO nudge for any outbox row written */
}

void reap_children(void) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        ChildProc *c = child_find(pid);
        if (!c) {
            /* Check if it's a channel process */
            channel_reap(pid, proc_db());
            continue;
        }

        if (c->type == CHILD_CRON_SCRIPT) {
            int64_t session_id = c->session_id;
            if (c->deadline == -1) { child_remove(c); continue; }  /* swept */
            char *hosts = NULL;
            char *output = child_output_finalize(c, status, &hosts);
            cron_script_post(c, output, hosts);
            free(output);
            child_remove(c);
            run_advance(session_id);
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

            char *hosts = NULL;
            char *output = child_output_finalize(c, status, &hosts);
            size_t out_len = strlen(output);

            /* afterToolCall hooks: chained result replacement, post-scanner,
             * pre-write (same contract as the inline dispatch path). The
             * replacement comes back already marker-sanitized (hook_dispatch
             * owns that invariant) — this reap path carries the
             * network_hosts-tagged results, so a transform hook must not be
             * able to smuggle a forged fence past storage-time sanitization. */
            char *hook_annotate = NULL;
            if (proc_tool_setup()) {
                char *rep = hook_dispatch_observe_tool_call(&proc_tool_setup()->ext_ctx,
                                proc_db(), c->tool_name, c->tool_args, output,
                                &hook_annotate);
                if (rep) { free(output); output = rep; out_len = strlen(output); }
            }

            /* CLI progress */
            if (!proc_is_daemon()) {
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
            int64_t rid = entry_append_with_iteration(proc_db(), session_id, &msg, c->iteration_id);
            if (hosts && rid > 0)
                db_entry_set_network_hosts(proc_db(), rid, hosts);
            db_tool_call_complete_with_result(proc_db(), c->entry_id, c->tool_call_id, rid);
            LOG_INFO_("tool done tool=%s is_err=%d", c->tool_name, is_err);
            LOG_TRACE_("tool result tool=%s len=%zu content=%s",
                       c->tool_name, out_len, output);
            if (hook_annotate) {
                if (rid > 0) hook_entry_data_patch(proc_db(), rid, hook_annotate);
                free(hook_annotate);
            }
            free(stored);
            free(output);
            child_remove(c);
            run_advance(session_id);
        }
    }
    /* A tool slot may have freed — re-advance any ceiling-stalled sessions. */
    stalled_drain();
}
