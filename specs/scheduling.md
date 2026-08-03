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
| `agent_max_per_parent` | 8 | launch | `launch_agent` refuses (fairness — one parent can't monopolize the fleet) |
| `session_max_active` | 20 | launch | `launch_agent` refuses; cron `target:"new"` fires skip + log. Counts non-idle sub-agent sessions (`session_count_active_agents`) — the existence half |
| `session_max_concurrent` | 10 | drain | autonomous turn opens defer — **all** sessions, not just sub-agents (a cron storm and a fan-out storm load the box identically) |

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
