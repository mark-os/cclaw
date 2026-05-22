# Plan: Session Robustness + Inbox/Pending Model

Date: 2026-05-21

## Motivation

CClaw sessions can go off the rails in several ways that are hard to recover
from. Additionally, the current sub-agent result delivery (V13: explicit
`check_agent` only) requires the LLM to remember to poll — unreliable. We can
solve both problems with SQLite primitives: atomic CAS for session locking,
a new inbox table for cross-session message injection, and turn-level
completeness tracking.

This plan is additive — existing agent loop, CLI, Telegram, cron, and heartbeat
continue working. No rewrites.

---

## Part 1: Session Robustness

### Problem: Crash Mid-Turn

If the process crashes after appending an assistant message with tool_calls but
before all tool results are appended, the session is in an unrecoverable state.
The LLM sees dangling tool calls with no results and errors or hallucinates.

Variants:
- Crash after assistant+tool_calls, before any tool result
- Crash after N of M tool results (partial)
- Crash during LLM HTTP call (no response appended — benign, just retry)

### Problem: Concurrent Session Access

Two threads/processes can run agent_run on the same session simultaneously:
- Cron job fires while heartbeat is injecting
- User sends Telegram message while cron is mid-turn
- Two cron jobs target the same session

Result: interleaved entries, corrupted context.

### Problem: Runaway / Stuck Sessions

- Agent hits max_iterations, last entry is a tool result → orphaned
- Agent loop returns -1 (error), session left in ambiguous state
- Sub-agent process hangs forever (no timeout enforcement yet)

### Solution: Session State Machine + Turn IDs

#### Schema Changes

```sql
ALTER TABLE sessions ADD COLUMN state TEXT NOT NULL DEFAULT 'idle';
-- Valid states: 'idle', 'pending', 'running', 'error'

ALTER TABLE sessions ADD COLUMN locked_by TEXT;
-- Process identifier: "pid:1234" or "cli" or "cron:7"

ALTER TABLE sessions ADD COLUMN locked_at INTEGER;
-- Unix timestamp of lock acquisition

ALTER TABLE sessions ADD COLUMN error_count INTEGER NOT NULL DEFAULT 0;
-- Consecutive crash count. Reset to 0 on successful turn.
-- Janitor stops auto-recovering when >= MAX_AUTO_RECOVERY (default 3).

ALTER TABLE entries ADD COLUMN turn_id INTEGER;
-- Groups all entries from one agent_run iteration
-- NULL for legacy entries (pre-migration)

CREATE INDEX IF NOT EXISTS idx_entries_session_turn ON entries(session_id, turn_id);
-- Composite index: O(1) lookup of max turn_id per session (index-only scan)
-- Next turn_id derived at start of turn:
--   SELECT COALESCE(MAX(turn_id), 0) + 1 FROM entries WHERE session_id = ?
```

#### State Transitions

```
idle ──→ pending    (inbox message arrives)
idle ──→ running    (direct agent_run, e.g. CLI input)
pending ──→ running (session runner picks up)
running ──→ idle    (agent_run completes successfully)
running ──→ error   (agent_run fails or crash detected)
error ──→ idle      (manual recovery or auto-heal)
```

#### Session Locking (CAS)

```c
int session_try_acquire(sqlite3 *db, int64_t session_id, const char *owner) {
    const char *sql =
        "UPDATE sessions SET state='running', locked_by=?, locked_at=unixepoch()"
        " WHERE id=? AND state IN ('idle','pending');";
    // returns 1 if sqlite3_changes()==1, else 0
}

void session_release(sqlite3 *db, int64_t session_id) {
    "UPDATE sessions SET state='idle', locked_by=NULL, locked_at=NULL"
    " WHERE id=? AND state='running';";
}
```

Single atomic UPDATE — no race conditions. If two processes try to acquire the
same session, exactly one succeeds (SQLite serializes writes).

#### Stale Lock Detection

In the daemon main loop (janitor sweep), periodically:

```sql
UPDATE sessions SET state='error', locked_by=NULL, locked_at=NULL,
  error_count = error_count + 1
  WHERE state='running' AND locked_at < unixepoch() - 300;
```

300s is generous — a normal turn takes <60s. Configurable.

#### Crash Loop Prevention

If a session crashes due to a deterministic bug (malformed inbox message,
buffer overflow in context_build), the janitor would blindly reset it to
'pending', the next consumer hits the same bug, crashes, and loops forever.

Fix: add `error_count` to sessions. Janitor only auto-recovers if below
a threshold:

```sql
ALTER TABLE sessions ADD COLUMN error_count INTEGER NOT NULL DEFAULT 0;
```

Janitor logic:
```c
#define MAX_AUTO_RECOVERY 3

// Only auto-recover if error_count < threshold
// Sessions at or above threshold stay in 'error' permanently
UPDATE sessions SET state='pending', locked_by=NULL, locked_at=NULL
  WHERE id=? AND state='error' AND error_count < MAX_AUTO_RECOVERY
  AND EXISTS (SELECT 1 FROM inbox WHERE session_id=? AND consumed_at IS NULL);
```

On successful turn completion, reset the counter:
```sql
-- Inside session_release, on clean exit to 'idle':
UPDATE sessions SET state='idle', locked_by=NULL, locked_at=NULL, error_count=0
  WHERE id=? AND state='running';
```

Behavior:
- 1st crash: janitor recovers, error_count=1
- 2nd crash: janitor recovers, error_count=2
- 3rd crash: janitor recovers, error_count=3
- 4th crash: error_count=3 ≥ MAX, janitor leaves it in 'error'
- Requires manual CLI intervention (or explicit reset command)
- Any successful turn resets error_count to 0

This ensures transient failures (power loss, OOM) recover automatically while
deterministic bugs get quarantined after a few attempts.

#### Turn Completeness

Each iteration of the agent loop assigns a turn_id (monotonically increasing
per session). All entries created in that iteration share the turn_id.

A turn is **complete** if:
- It ends with an assistant message with no tool_calls (final answer), OR
- Every tool_call in the assistant message has a matching tool result entry

`context_build` detects incomplete final turns and injects a system notice:
"[Previous turn was interrupted. Tool call results may be missing. You may
retry the operation if needed.]"

This gives the LLM awareness of what happened and a chance to retry. Silently
skipping the turn would lose context about what the agent was trying to do.

#### Error Recovery

Sessions enter 'error' state when the janitor detects a stale lock (process
crashed while holding it). Recovery is straightforward because `context_build`
already handles the data side (incomplete turn notice).

**Automatic (janitor):**
```sql
-- Janitor checks: does this errored session have pending inbox messages?
UPDATE sessions SET state='pending', locked_by=NULL, locked_at=NULL
  WHERE id=? AND state='error'
  AND EXISTS (SELECT 1 FROM inbox WHERE session_id=? AND consumed_at IS NULL);

-- If no inbox messages, just go idle:
UPDATE sessions SET state='idle', locked_by=NULL, locked_at=NULL
  WHERE id=? AND state='error'
  AND NOT EXISTS (SELECT 1 FROM inbox WHERE session_id=? AND consumed_at IS NULL);
```

The next agent_run on this session will see the incomplete turn (entries with
the crashed turn_id that lack a final assistant response), and `context_build`
injects the system notice. The LLM decides whether to retry.

**Manual (CLI):**
User resumes the session. CLI shows a warning: "session recovered from error —
previous turn was incomplete." Then acquires normally. Same `context_build`
notice gives the LLM awareness of what happened.

No special repair step is needed. The 'error' state is just a flag meaning
"last run crashed." The entries table has the evidence (incomplete turn_id),
and `context_build` handles it regardless of how the session got back to
'idle' or 'pending'.

---

## Part 2: Inbox / Pending Model

### Concept

Any part of the system can inject a message into any session's inbox. The
session runner (or CLI, or Telegram handler) consumes inbox messages at the
start of a turn. This decouples "something happened" from "the agent processes
it."

### Schema

```sql
CREATE TABLE IF NOT EXISTS inbox (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id INTEGER NOT NULL REFERENCES sessions(id),
    source TEXT NOT NULL,
    -- Format: 'user', 'telegram:12345', 'subagent:42', 'cron:7', 'system'
    content TEXT NOT NULL,
    priority INTEGER NOT NULL DEFAULT 0,
    -- 0=normal, 1=high (sub-agent result), -1=low (heartbeat notice)
    created_at INTEGER NOT NULL DEFAULT (unixepoch()),
    consumed_at INTEGER
    -- NULL = pending, set when injected into session entries
);
CREATE INDEX IF NOT EXISTS idx_inbox_pending
  ON inbox(session_id) WHERE consumed_at IS NULL;
```

### Core Operations

```c
// Insert a message into a session's inbox
int64_t inbox_insert(sqlite3 *db, int64_t session_id,
                     const char *source, const char *content, int priority);

// Consume all pending inbox messages for a session AND append as entries.
// MUST be a single transaction (BEGIN EXCLUSIVE ... COMMIT):
//   1. SELECT unconsumed messages
//   2. INSERT into entries (with turn_id, source attribution)
//   3. UPDATE inbox SET consumed_at = unixepoch() WHERE id IN (...)
// If the process crashes mid-transaction, SQLite rolls back — no messages lost.
// Returns count of messages consumed.
int inbox_consume_into_entries(sqlite3 *db, int64_t session_id, int64_t turn_id);
```

**Critical invariant**: marking consumed and appending entries is atomic. If
these were separate operations, a crash after `UPDATE consumed_at` but before
`INSERT INTO entries` would permanently lose the message. A single transaction
guarantees both happen or neither does.

### Injection Format

When inbox messages are consumed, they become entries in the session:

```c
// For user-originated messages:
entry_append(db, session_id, &(Message){.role = ROLE_USER, .content = msg.content});

// For system/subagent/cron messages:
entry_append(db, session_id, &(Message){.role = ROLE_SYSTEM, .content = formatted});
// Where formatted = "[from subagent:42] Task completed. Result: ..."
```

The source attribution lets the LLM understand where the message came from
without needing a tool call.

---

## Part 3: Sub-Agent Wake

### Current Flow (V13)

```
parent spawns sub-agent → sub-agent runs → writes result to sub_agents table
parent must call check_agent tool to see result
```

Problem: parent only checks if the LLM decides to. If the parent session is
idle (user walked away), the result sits unprocessed forever.

### New Flow (Inbox Wake)

```
parent spawns sub-agent → sub-agent runs → writes result to sub_agents table
                                         → inbox_insert(parent, "subagent:N", result)
                                         → session_mark_pending(parent)
session runner sees pending parent → acquires → consumes inbox → agent processes result
```

The sub-agent's `run_sub_agent()` epilogue becomes:

```c
subagent_finish(db, agent_id, "done", result);

// Inject into parent's inbox
char source[32];
snprintf(source, sizeof(source), "subagent:%lld", (long long)agent_id);
inbox_insert(db, parent_session_id, source, result, 1);  // priority=1 (high)
session_mark_pending(db, parent_session_id);
```

### Delivery Policy

Add a column to control behavior per sub-agent spawn:

```sql
ALTER TABLE sub_agents ADD COLUMN on_complete TEXT NOT NULL DEFAULT 'inbox';
```

Values:
- `'inbox'` — inject result into parent inbox, mark pending (new default)
- `'silent'` — just mark done, parent must check_agent (current V13 behavior)
- `'spawn'` — auto-spawn a follow-up sub-agent with the result (pipeline mode)

The `spawn_agent` tool gains an optional `on_complete` parameter. Default is
'inbox' which is the safe, useful behavior. Power users can pass 'silent' for
fire-and-forget background work.

### check_agent Still Works

`check_agent` remains useful for:
- Checking status mid-turn (is it done yet?)
- Getting results from 'silent' sub-agents
- Explicit polling in long-running orchestration

The inbox wake is complementary, not a replacement.

---

## Part 4: Decentralized Execution (Daemon Mode)

### Concept

Every thread that produces work can also execute it. The CAS lock prevents
concurrent access. The inbox is the durable coordination mechanism. No central
runner thread needed.

Universal pattern for all threads:

```c
// Any thread wanting to process a session:
if (session_try_acquire(db, session_id, my_id)) {
    inbox_consume_into_entries(db, session_id);
    agent_run(&ctx);
    // Release checks inbox — if non-empty, stays 'pending' not 'idle'
    session_release(db, session_id);
    // Optionally: if still pending, run again immediately
}
// If acquire fails: no problem, whoever holds it will see the inbox msg
```

### Janitor Sweep (Main Loop)

The daemon's existing `while (!shutdown) { sleep(1); subagent_reap(); }` loop
gains a janitor check:

```c
while (!shutdown_requested()) {
    sleep(1);
    subagent_reap(db);
    janitor_sweep(db, cfg);  // pick up orphaned pending sessions
}
```

The janitor handles edge cases:
- Sessions stuck in 'pending' because no thread noticed
- Stale locks (running for >300s, process probably crashed)

### Integration with Existing Channels

- **CLI**: Acquires session directly. Consumes inbox on entry.
- **Telegram**: On message, inbox_insert + try to acquire and run directly.
  If acquire fails (session busy), message waits in inbox.
- **Cron**: On due job, inbox_insert + try to acquire and run directly.
- **Heartbeat**: Injects system msg directly (no inbox needed — it's
  informational, not a turn trigger).
- **Sub-agent finish**: inbox_insert into parent + session_mark_pending.
  Janitor or next thread to touch the session picks it up.

---

## Part 4b: The Release Phase (Critical Detail)

### The Race Condition

Naive release creates a TOCTOU window:

```
Thread A (holding lock)                Thread B (incoming msg)
-----------------------                -----------------------
1. agent_run finishes
2. Checks inbox → 0 msgs
                                       3. inbox_insert → success
                                       4. session_mark_pending → FAILS (state='running')
5. SET state='idle'
```

Result: message in inbox, session idle, stalls until janitor sweep.

### The Fix: Atomic Release with BEGIN IMMEDIATE

The release must check inbox and set state inside a single write transaction.
`BEGIN IMMEDIATE` acquires SQLite's write lock, preventing any other connection
from inserting during the check.

### The Keep-Alive Loop

Rather than releasing to 'pending' (which requires someone else to pick it up),
the holding thread loops if there's more work:

```c
int max_consecutive_turns = 5;  // prevent starvation
int turns = 0;

while (1) {
    inbox_consume_into_entries(db, session_id);
    agent_run(&ctx);
    turns++;

    sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
    int unconsumed = get_unconsumed_count(db, session_id);

    if (unconsumed == 0 || turns >= max_consecutive_turns) {
        // Release — go idle or pending depending on remaining work
        const char *new_state = (unconsumed > 0) ? "pending" : "idle";
        exec_release(db, session_id, new_state);
        sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
        break;
    } else {
        // More work arrived during agent_run — keep lock, loop immediately
        sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
        continue;
    }
}
```

### Why This Works (All Cases)

- **Thread B arrives before transaction**: `unconsumed > 0`, Thread A keeps
  lock and processes immediately (<1ms latency).
- **Thread B arrives during transaction**: `BEGIN IMMEDIATE` blocks Thread B's
  `inbox_insert` until Thread A commits. Thread A sees 0, goes idle. Thread B's
  insert then succeeds, Thread B acquires the now-idle session.
- **Thread B arrives after transaction**: Session is idle, Thread B acquires
  normally.

Zero dropped messages. Zero latency penalties for the common case.

### Starvation Prevention

The `max_consecutive_turns` cap (default 5) prevents a single session from
hogging a thread indefinitely. After N turns, the thread releases even if
there's more work. The session goes to 'pending' and gets picked up on the
next cycle.

This matters for:
- **Telegram thread**: Must return to `getUpdates` to serve other chats
- **Cron thread**: Must check other due jobs
- **Sub-agent pipelines**: Where each completion triggers another spawn

With cap=5 and typical 5-10s turns, worst case is 25-50s before the thread
is free. For interactive channels (Telegram), cap=3 may be more appropriate.

The cap is configurable per-caller:
- CLI: no cap (user is waiting, only one session)
- Telegram: cap=3 (must poll other chats)
- Cron: cap=5 (less latency-sensitive)
- Janitor: cap=1 (just unstick things, don't hog)

---

## Part 5: Implementation Order

### Phase A: Robustness (no behavior change)

1. Add `state`, `locked_by`, `locked_at` columns to sessions
2. Add `turn_id` column to entries
3. Implement `session_try_acquire` / `session_release`
4. Modify `agent_run` to assign turn_ids
5. Modify `context_build` to detect incomplete turns + inject system notice
6. Add stale lock detection to main loop (janitor)
7. Tests: crash recovery, concurrent access rejection

### Phase B: Inbox

8. Create `inbox` table
9. Implement `inbox_insert`, `inbox_consume`, `session_mark_pending`
10. Modify sub-agent finish to use inbox_insert
11. Add `on_complete` column to sub_agents, update spawn_agent tool
12. Tests: inbox insert/consume, pending state transitions

### Phase C: Decentralized Acquire/Release

13. Wrap CLI agent_run in acquire/release + inbox consume
14. Wrap Telegram agent_run in acquire/release + inbox consume
15. Wrap Cron agent_run in acquire/release + inbox consume
16. Add janitor sweep to daemon main loop
17. Tests: end-to-end sub-agent → inbox → parent wake

### Phase D: Polish

18. Status page shows inbox depth per session + session states
19. CLI shows pending inbox messages on session resume
20. Add `inbox_list` tool (let agent see its own pending messages)
21. Configurable stale lock timeout

---

## Design Decisions

**Why not SQLite notify/hooks?** `sqlite3_update_hook` only fires in the same
connection. Cross-process notification requires polling or OS-level IPC. 1s poll
is simpler and sufficient.

**Why not a separate message queue?** SQLite IS the message queue. The inbox
table with consumed_at is a durable, transactional, queryable queue. Adding
Redis or similar would violate the "SQLite is the backbone" principle.

**Why state machine over advisory locks?** Advisory locks (flock/fcntl) don't
survive process crashes cleanly and aren't visible to other parts of the system.
The state column is queryable, debuggable, and crash-recoverable.

**Why keep check_agent?** Belt and suspenders. The inbox handles the common case
(sub-agent finishes while parent is idle). check_agent handles the power case
(parent is actively orchestrating and wants to poll mid-turn). Both are cheap.

**Why 1s poll, not instant wake?** Instant wake requires either:
- Shared memory / condvar (complex, not cross-process)
- Unix signals (fragile)
- inotify on the DB file (fires too often, not message-level)
- A pipe/socket between processes (adds IPC complexity)

1s is imperceptible for async sub-agent results. Interactive channels (CLI,
Telegram) bypass the runner and acquire directly.

---

---

## Discussion: What Is a Session? What Is an Agent?

### Session

A session is a **passive data structure** — a linked list of entries (leaf →
root via parent_id) stored in SQLite. It represents one conversation history.
It is roughly equivalent to the "context window" source material, though not
all of it gets sent to the model on any given turn.

Key properties:
- Entries include tool calls and tool results even if they're later trimmed
  from what's sent to the LLM (context windowing)
- The session is loaded from DB into a heap-allocated `Entry[]` on demand
- After processing, the `Entry[]` is freed — nothing persists in memory
- Sessions never truly "end" — they have completion points (idle states)

### Agent

An agent is an **ephemeral runtime** — a set of rules for how to construct an
LLM message and what to do with the response. It consists of:
- A system prompt (persona, rules)
- A tool set (what it can do)
- A workspace (where file tools operate, optional)
- A model/provider config (what LLM to call)
- Iteration limits, token budgets

**Sessions don't belong to agents. Agent runs operate on sessions.** The same
session could theoretically be processed by different agent configurations.
In practice, sessions tend to be associated with one agent config, but this is
incidental — not enforced by the data model.

### Memory Model (Current)

Each iteration of the agent loop already does load-process-free:
1. `session_get_branch()` → heap-allocated `Entry[]`
2. `context_build()` → heap-allocated `Message[]` (subset of entries)
3. `llm_build_request()` → arena-allocated JSON string
4. HTTP call, parse response (arena)
5. Append new entries to DB
6. Free `Entry[]`, free `Message[]`, destroy arena
7. Next iteration starts fresh

The session is never "held" in memory across turns. SQLite's page cache is the
only persistent in-memory representation, and it's managed by SQLite itself.

---

## Discussion: Central Runner vs. Decentralized Execution

### The Central Runner Model (from original plan)

One dedicated thread processes all pending sessions:
- All sources (telegram, cron, sub-agents) just insert into inbox
- Runner polls for pending sessions, acquires, runs, releases
- Natural backpressure: sessions queue up, processed one at a time

Pros: single point of execution, easy to reason about, bounded memory.
Cons: adds latency for interactive channels, single point of failure.

### The Decentralized Model (preferred)

Any thread can call agent_run. The CAS lock prevents concurrent access.
The inbox is the durable coordination mechanism.

Pattern for every thread:
```c
if (session_try_acquire(db, session_id, my_id)) {
    inbox_consume_into_entries(db, session_id);
    agent_run(&ctx);
    // Release checks inbox — if non-empty, stays 'pending' not 'idle'
    session_release(db, session_id);
}
// If acquire fails: whoever holds it will see the inbox msg when they finish
```

**Why this works without races:**

1. Agent A finishes, writes to parent's inbox, marks parent pending
2. Parent is currently running (state='running') — `session_mark_pending`
   CAS fails (only transitions idle→pending). That's fine.
3. The inbox message is durable in SQLite. It cannot be lost.
4. When parent's current agent_run finishes and calls `session_release`:
   - Release checks: "any unconsumed inbox messages for this session?"
   - If yes: transition to 'pending' instead of 'idle'
   - The releasing thread (or any thread that notices) picks it up immediately
5. If no thread notices (edge case): janitor sweep catches it

**The inbox is the durable queue. The session state is an optimization hint.**
Even if state gets stuck, the next time anything touches that session it can
check for unconsumed inbox items.

### Janitor Thread (Optional Safety Net)

Not the primary executor — just catches orphaned pending sessions:

```c
// Runs every 5s in daemon mode:
while (running) {
    sleep(5);
    // Find sessions in 'pending' state for >2s with no lock holder
    // Also: detect stale locks (running for >300s)
    sweep_orphaned_sessions(db, cfg);
}
```

This replaces the "central runner" concept. The main loop in daemon mode
already has a `sleep(1) + subagent_reap()` cycle — the janitor logic fits
there naturally.

### Decision

**Use decentralized execution with CAS locking.** No central runner needed.
Every thread that produces work can also execute it (or leave it for whoever
finishes next). The janitor sweep in the main loop handles edge cases.

---

## Discussion: The Daemon

### Current Structure

Daemon mode (`./build/cclaw` without `--cli`) is a long-running process:

```
main() {
    open DB
    start telegram poller thread   ← long-polls getUpdates, calls agent_run
    start web server thread        ← civetweb, serves status page
    start heartbeat thread         ← injects system msgs every N seconds
    start cron thread              ← checks due jobs every 60s, calls agent_run

    while (!shutdown) {
        sleep(1);
        subagent_reap(db);         ← waitpid on child processes
    }

    stop all threads, close DB
}
```

### Proposed Structure (with inbox + CAS)

```
main() {
    open DB
    start telegram poller thread   ← receives msgs, inbox_insert, acquire+run
    start web server thread        ← status page (read-only, shows inbox depth)
    start heartbeat thread         ← injects system msgs (unchanged)
    start cron thread              ← job due → inbox_insert, acquire+run

    while (!shutdown) {
        sleep(1);
        subagent_reap(db);         ← reap zombies, mark crashed
        janitor_sweep(db, cfg);    ← pick up orphaned pending sessions
    }

    stop all threads, close DB
}
```

The main change: each thread uses the acquire/release pattern, and the main
loop gains a janitor sweep. No new threads needed.

---

## Impacted Files

### Must Change

| File | Change |
|------|--------|
| `src/db.c` | Add inbox table, session state/lock columns, turn_id column to schema. Add `session_try_acquire`, `session_release`, `session_mark_pending`, `inbox_insert`, `inbox_consume` functions. |
| `include/db.h` | Declare new DB functions, InboxMsg struct |
| `include/types.h` | Add session state enum, possibly InboxMsg type |
| `src/agent.c` | Assign turn_id per iteration. Wrap loop body in acquire/release (or expect caller to have acquired). |
| `src/context.c` | Detect and skip incomplete turns (check turn_id completeness) |
| `src/tool_subagent.c` | On sub-agent finish: `inbox_insert` into parent + `session_mark_pending` |
| `src/main.c` | Add janitor sweep to main loop. Update sub-agent finish path. |

### Likely Change

| File | Change |
|------|--------|
| `src/cli.c` | Use `session_try_acquire`/`session_release` around agent_run. Consume inbox on session resume. |
| `src/telegram.c` | Use acquire/release pattern. Insert via inbox or acquire directly. |
| `src/cron.c` | Use acquire/release pattern. Insert via inbox or acquire directly. |
| `src/heartbeat.c` | Minor: could use inbox for heartbeat msgs, or keep direct injection (simpler). |
| `include/agent.h` | Possibly add turn_id to AgentContext |

### Possibly Change

| File | Change |
|------|--------|
| `src/web.c` | Show inbox depth, session states on status page |
| `include/context.h` | If context_build signature changes for turn completeness |
| `src/tool_subagent.h` | If SubAgentCtx gains parent_session_id for inbox delivery |

### No Change Expected

| File | Reason |
|------|--------|
| `src/arena.c` | Memory model unchanged |
| `src/config.c` | No new config fields needed (maybe stale_lock_timeout later) |
| `src/http.c` | Unrelated |
| `src/llm.c` | Unrelated |
| `src/shutdown.c` | Unrelated |
| `src/tools.c` | Registry unchanged |
| `src/tool_shell.c` | Unrelated |
| `src/tool_file.c` | Unrelated |
| `src/tool_js.c` | Unrelated |
| `src/tool_cron.c` | Unrelated |

---

## Spec Impact

New invariants to add to §V:
- V16: ∀ agent_run → session must be in 'running' state (acquired via CAS)
- V17: ∀ turn → entries share a turn_id; incomplete turns skipped on context build
- V18: ∀ inbox message → consumed exactly once, in priority then FIFO order
- V19: ∀ session state transition → via atomic UPDATE with WHERE guard

New tasks for §T (after current T54):
- T55–T61 map to Phase A (robustness)
- T62–T66 map to Phase B (inbox)
- T67–T70 map to Phase C (session runner → janitor sweep)
- T71–T74 map to Phase D (polish)
