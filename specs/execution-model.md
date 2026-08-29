# Tool execution model — vehicles × delivery × concurrency

The one model behind every tool call, sub-agent, and background job. Design
history and rationale: `plan/projects/blocking-vs-background-tools.md`
(ratified 2026-07-31); turn/iteration/batch vocabulary: AGENTS.md "Turn Model
Terminology".

## The two orthogonal axes

Every tool call has:

- **delivery** — `turn` (the result closes the call inside the turn; the
  default) or `job` (the call closes immediately with a synthetic handle;
  the real result arrives later as an inbox notice → a role-1 user entry at
  the next turn boundary, never as a tool message).
- **concurrency** — `serial` (default: dispatch stops until the call
  completes) or `parallel` (sibling calls in the same batch keep
  dispatching).

`launch_agent` was the first tool exposing both (its `background` argument
selects delivery; it is parallel-safe); `shell_exec` is the first process
tool with selectable delivery (`background: true`).

## Execution vehicles (`ToolRecipe.vehicle`, tools.h)

| Vehicle | Runs on | Tools | Notes |
|---|---|---|---|
| `EXEC_INLINE` | event-loop thread | control-flow tools (launch_agent, check_session, cancel, request_config, cron/extension/agent tools, channel_send) | synchronous unless the recipe's `null_kind` says otherwise |
| `EXEC_THREAD` | detached thread | DB/session-only tools (memory_*, cron_list/remove, db_query, search_config/models) | call already `running`; completion writes the result |
| `EXEC_SANDBOX` | fork+exec `--run-tool` child (`CHILD_MAX` 48) | file / shell / web / js tiers | result pipe drained by the poll loop; reap completes the call |

## Traits live in the recipe

`ToolRecipe` (tools.h) is the single declaration site, set at each tool's
registration; the dispatcher reads `te->recipe`. Extension-defined tools get
the safe defaults by construction.

| Trait | Default | Meaning |
|---|---|---|
| `parallel_safe` | 0 (serial) | models emit ordered calls — shell especially — expecting sequential side effects; only independent self-contained delegations (launch_agent) opt in |
| `needs_interp` | 0 | `{{SECRET:X}}` resolved to real values at exec time (shell_exec, web_fetch, js_eval) |
| `backgroundable` | 0 | accepts `background: true` (shell_exec, launch_agent) |
| `null_kind` | `NULL_NONE` | EXEC_INLINE NULL-return protocol: `NULL_ASYNC` (sub-agent launched; continue per parallel_safe), `NULL_PARK` (approval gate) |

## The dispatch loop return codes

`1` inline done → next call · `3` parallel-safe async → next call ·
`0` serial async → stop, wait · `2` parked for approval → unclaim, stop ·
`-1` child ceiling → unclaim, stalled, re-advance on reap.

**Turn-join**: the next LLM request is built only when no call of the batch
is `pending` or `running`. Status `background` is invisible to both the
turn-join (`any_running`) and re-dispatch (`get_pending`) — that is the
entire mechanism by which a job leaves the turn.

## Background jobs (`status='background'`)

**The job IS the `tool_calls` row**: `job_id` = rowid. No jobs table — jobs
cannot survive the daemon (forked children die with it), so durable job
state would be a lie.

1. **Start**: `background: true` on a backgroundable tool. Dispatch is
   identical through the fork (gate, secrets, sandbox); then the dispatcher
   answers the call with `"job started (job_id=N, log=…)"` and sets status
   `background`, stamping the dispatching `instance_id` in `resolved_by`.
   Timeout: `timeout` argument, default 600s, ceiling `job_timeout_max`
   (config, default 3600) — not the turn clamp.
2. **Live output**: the drain appends the child's pipe output to
   `<workspace>/.tool_results/<session>/<call_id>.log` — bounded by disk,
   readable live from inside the sandbox (`tail`/`file_read`; the same
   `.tool_results` home as spill files). The log carries stdout-level trust
   only: it is rw inside the job's own sandbox. `ps` structurally cannot
   see jobs — each runs in its own PID namespace.
3. **Ambient status**: a `<background_jobs>` section in the frozen turn
   context (id, command snippet, age, log path, howto). Frozen with the
   block — mid-turn freshness is pull-based (log, `check_session`).
4. **Completion**: reap runs the shared scrub (see below) over the log
   tail, posts one inbox notice (`source='job_result'`), flips the row
   `done` with `resolved_by='job:exit=N' | 'job:signal=N' | 'job:timeout'
   | 'job:cancelled'`, and wakes the session. The notice enters as a
   user entry at the next boundary — the mid-turn invariant holds.
5. **Timeout/cancel**: the deadline sweep posts the notice itself (there is
   no tool result to write) and marks the slot swept; `cancel {job_id}`
   SIGTERMs, then the sweep SIGKILLs after 5s.
6. **Restart**: recovery closes `background` rows whose stamped instance is
   no longer registered (`job:orphaned` + inbox notice) — the model is
   never left believing a job survived. A live peer's jobs are spared by
   the stamp.

**Eligibility**: shell_exec, launch_agent. Approval-parking tools
(request_config, create_agent, …) never background — NULL_PARK and job
delivery compose into nothing sensible. Approval-*gated* calls park first;
backgrounding happens only when the call actually dispatches.

## One result-ingestion path (step 3)

Every completed tool result takes the same tail into the session —
`tool_result_commit` (dispatch.c): CLI arrow, UTF-8 sanitize,
truncate/spill, entry write (+ network-hosts tag), call completion, hook
patch. The security half (secret capture → deinterpolate → scan/redact →
hooks) runs before it, per origin. External-origin results — a sub-agent's
final text entering its parent, a job's log tail — pass
`tool_result_scrub` (secret_store.c) and the same truncate/spill policy, so
there is no second, weaker writer of results.

## Introspection and control

- `check_session {session_id}` — child state; final text once idle
  (consumed-by-poll advances the delivery cursor so the push is not
  repeated); a compacted transcript tail while running.
- `check_session {job_id}` — job state, runtime, log path, output tail.
- `check_session {}` — your live child sessions and background jobs.
- `cancel {job_id}` — stop a job. Session cancel is deliberately deferred
  (delivery-semantics M2); a child's turn bounds end it on their own.
- `db_query` stays the sanctioned general hatch; check_session is the
  ergonomic ownership-scoped view.

## Caps (the real rails)

`agent_max_depth` 2 · `AGENT_MAX_PER_PARENT` 8 · `AGENT_MAX_TOTAL` 16 ·
`CHILD_MAX` 48 · `job_timeout_max` 3600s · `max_iterations` per turn ·
`max_autonomous_turn_streak` cross-turn. The worker tool filter narrows
visibility, never authority (grants ∩ filter, both enforced at dispatch);
the default worker list is rendered into launch_agent's description so a
spawner can read what a worker will get.
