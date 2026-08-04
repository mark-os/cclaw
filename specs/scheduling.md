# Scheduling — Session Concurrency Gates

Status: shipped 2026-08-03. Design history in
`plan/projects/session-concurrency-gates.md` (agreed spec, same date).

Related: [delivery.md](delivery.md) — what a session's turn boundaries *ship*
(per-edge delivery policy, quiescence) lives there; both contracts meet at
`advance_session`'s choke points.

The old `AGENT_MAX_TOTAL` conflated two protections: **existence** (how many
delegation-tree sessions may be in flight — guards runaway spawning) and
**execution** (how many turns run at once — guards the box). They are now two
gates with different semantics at different choke points. The test for every
rule: forgetting it must *degrade*, never corrupt.

## The three rules

1. **Producers with a synchronous caller pass the launch gate.** Refuse/skip
   with feedback *before* anything is created. A closed set: `launch_agent`
   (error string naming the knob, so the model can report it) and
   session-creating cron fires (`target:"new"` — skip + log, the next
   schedule retries; a skip is not a failure and never feeds the 3-strike
   auto-pause). Channel routes that create sessions have no synchronous
   caller and are never refused — the session row is cheap; rule 3 governs
   the expensive part.
2. **The inbox accepts everything, always.** Insertion is a delivery promise;
   rows are durable and eventually drained. No inbox-insert gate, no inbox
   cap.
3. **The drain gate defers, never refuses, and orders by the human bit.** By
   drain time nobody is present to hear a refusal. Over the concurrency cap
   an autonomous turn simply doesn't open — the inbox rows stay queued
   (streak-guard shape) — while a batch with a human anywhere in it opens
   even over the cap. Enforced in `advance_session`'s idle branch and its
   compacting twin, before the drain, so a deferral leaves the queue intact.

## Knobs (config registry; 0 = unlimited)

| Key | Default | Gate | Over the cap |
|-----|---------|------|--------------|
| `session_max_active` | 20 | launch | `launch_agent` refuses; cron `target:"new"` fires skip + log. Counts in-flight sub-agent sessions (`session_count_active_agents`) — the existence half |
| `session_max_concurrent` | 10 | drain | autonomous turn opens defer — **all** sessions, not just sub-agents (a cron storm and a fan-out storm load the box identically) |

Two knobs, one per back-pressure flavor: existence over `session_max_active`
is **refused** while a synchronous caller can hear it; execution over
`session_max_concurrent` **queues** (the inbox holds the work, the sweep
retries). Read together: 10 may execute, the other 10 are the allowed queue
depth. There is deliberately no per-parent fairness cap — one parent
monopolizing the fleet is not a current concern, and a third knob on the same
gate was the main source of confusion (removed 2026-08-04; it was
`agent_max_per_parent`). If domination becomes real, the right arm for a
per-parent counter is subtree non-quiescence (delegation *trees* in flight,
`session_subtree_quiescent`), not child state — an idle child waiting on its
own grandchildren has not finished.

## Batch semantics — refuse choices, defer weather

One assistant response's tool calls are persisted from the response and
dispatched strictly in call order; when a limit fires mid-batch, which of the
three possible behaviors applies is a rule, not an accident:

- **Capacity the model can choose differently** (the `session_max_active`
  launch gate) refuses **per call, in call order, with feedback** — admit up
  to the cap, refuse the rest. Partial admission is the contract: the model
  gets N individual results at the turn-join and reconciles, exactly as it
  does for any other partially failing batch. The counter is batch-aware: "in
  flight" includes an idle child with an unconsumed inbox row
  (`SESSION_IN_FLIGHT`, db.c), because every launch in a batch dispatches
  before the event loop advances any of the children it created — counting
  state alone, one big batch would bypass the cap that the same calls spread
  over iterations are refused by. All-or-nothing was rejected: it needs a
  batch-level pre-scan that doesn't exist (calls dispatch one at a time),
  punishes the legal prefix, and tells the model less than N results do.
- **Capacity only time fixes** (the fork child ceiling, the LLM worker pool,
  rate limit, disk floor) **defers silently, never refuses** — an error
  result would just make the model retry into the same wall. The fork
  ceiling unclaims the call and stops the rest of the batch
  (`stalled_add` + freed-slot re-advance); dispatch-level budgets re-park
  the whole turn. Accepted cost: head-of-line blocking for the batch tail.
- **Turn-open limits** (drain gate, streak guard) act before a batch exists
  and always cover the whole pending set.

## The counting rule (the deadlock trap)

The drain gate counts sessions **holding a resource**
(`session_count_resource_holders`, db.c): an `llm_jobs` row in flight (turn or
compaction — a worker thread is running or queued for it) or a `running`
`tool_calls` row for any real tool. It deliberately does **not** count
"non-idle sessions": a parent blocked on a blocking `launch_agent` sits in
`tool_running` consuming nothing, and if waiting parents held slots, nested
delegation at the cap would deadlock — parents hold every slot, children can
never start, parents never finish. Two exclusions make this true:

- `tool_calls.name = 'launch_agent'` is a wait on another session, not a
  resource; the child counts on its own if it is actually working.
- The session being gated is excluded from its own count — a stale row of its
  own must never wedge it out of a slot.

**Mid-turn resumes are exempt.** The role-3 unanswered-leaf resume re-opens a
turn that is already half-spent; gating it strands paid-for work. A role-1
leaf (refused-dispatch repark) is a turn that never dispatched: gated when the
entry is stamped autonomous, open when unstamped (only the drain stamps, so no
stamp means a human typed it — same classification the streak guard uses,
`CCLAW_AUTONOMOUS_SOURCES` in advance.h).

## Retry path

No new machinery. A deferred session keeps its unconsumed inbox rows /
unanswered leaf, so `session_sweep_inbox` (main.c, daemon tick) re-advances it
every `POLL_DB_INTERVAL` — the correctness backstop, shared with the streak
guard and refused-dispatch parks. For latency (and for the CLI, which has no
sweep), a deferral also lands the session in main.c's capacity-stall list
(`g_stalled`, the CHILD_MAX park pattern): a reap, LLM-worker completion, or
tool-thread completion — the events that free a resource — drains the list and
re-advances. Still over a ceiling → the session just re-enters it. The list is
an ephemeral scheduling hint, never a source of truth; overflow means
sweep-tick latency, nothing more.

Related fix, same shape: `llm_worker_submit` / `llm_worker_submit_compact`
delete their persisted `llm_jobs` row when `pool_push` fails (queue full). A
stale row reads as an in-flight request to `advance_session`'s `job_in_flight`
checks — the session would park in WAITING forever — and inflates the
resource-holder count.

## Observability

Deferral logs at debug (the sweep re-lands there every tick; an incident-grade
notice would spam). The visible symptom of a system running hot under
defer-only gates is inbox backlog depth per session — a `doctor` metric
candidate, not yet built.
