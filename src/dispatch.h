#ifndef CCLAW_DISPATCH_H
#define CCLAW_DISPATCH_H

/*
 * Tool dispatch: what the trusted parent does with a model-requested tool
 * call, from the gate that decides whether it may run at all through the
 * completion that turns a finished child's bytes back into a tool result.
 *
 * The gate (grants → session filter → policy → hooks → sensitivity/secret
 * bind → approval) and the per-call egress computation are why this module
 * exists as one place: they are the single choke point where the trusted
 * parent decides what a sandboxed child is allowed to do and reach. child.c
 * only forks, tracks, and drains — it never decides.
 *
 * Also here: the LLM request path (the other thing run_advance dispatches),
 * the capacity-stalled session set, and the cron fire pipeline, which is a
 * JS-tier dispatch with a schedule instead of a tool_call in front of it.
 *
 * Lib-resident: loop.c's run_advance dispatches through here on every turn,
 * so dispatch.o cannot live in main.o (which the Makefile filters out of
 * libcclaw.a).
 */

#include <stddef.h>
#include <stdint.h>

#include "cron.h"
#include "db.h"

/* Execute one claimed tool call. Returns 1 = inline (result written),
 * 3 = parallel-safe async dispatched, 0 = serial async dispatched (caller
 * waits), 2 = parked awaiting approval, -1 = child ceiling hit (caller
 * unclaims and stalls the session). */
int dispatch_tool(int64_t session_id, const char *agent_name,
                  PendingToolCall *tc);

/* Submit this session's next LLM request to the worker pool, after the
 * max-iteration, rate-limit, cost-ceiling and disk-floor gates. Returns the
 * submit result, or -1 when a gate deferred or refused the request. */
int dispatch_llm_req(int64_t session_id, const char *agent_name, int iteration);

/* Remember a session parked on a capacity ceiling so a freed resource
 * re-advances it. Deduped; overflow just costs sweep-tick latency. */
void stalled_add(int64_t session_id);

/* Re-advance every session stalled on a ceiling. Called wherever a resource
 * frees: reap, worker completion, tool-thread completion. */
void stalled_drain(void);

/* Reap every exited child and route its output: tool result, cron result, or
 * a channel process handed back to channel.c. */
void reap_children(void);

/* SIGKILL children past their deadline and write the timeout result. */
void child_sweep_deadlines(void);

/* cron.c's script dispatcher (registered with cron_set_script_runner): run a
 * scheduled script as the target agent, in exactly the JS-tier sandbox a
 * js_eval tool call would get. */
CronScriptRc cron_script_run(const CronScriptFire *f, char *err, size_t err_len);

/* ── INTERIM: main.c-resident callee of the above ───────────────────
 * Still a static-turned-extern in main.c; dispatch.o carries an undefined
 * reference to it that only resolves at the final cclaw link. It moves to a
 * real module later in PROJECT:main-c-reorg — when it does, delete this
 * declaration and include that module's header instead. Nothing else should
 * declare or call it.
 */
void handle_approval_park(int64_t session_id);      /* → approval.c (step 8) */

#endif
