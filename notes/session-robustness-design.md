# Technical Design Document: Session Robustness & Transactional Inbox Architecture

---

## 1. System Architecture Overview

The CClaw concurrency and fault-tolerance model shifts from non-isolated, ad-hoc execution to a **decentralized, state-machine-driven transactional system** powered by SQLite primitives.

```
   [Telegram Thread]      [Cron Thread]     [Sub-Agent Process]
          │                     │                     │
          ▼                     ▼                     ▼
    inbox_insert()        inbox_insert()        inbox_insert()
          │                     │                     │
          └──────────────┬──────┴─────────────────────┘
                         ▼
                ┌────────────────┐
                │  SQLite INBOX  │ ◄─── Durable Queue
                └───────┬────────┘
                        │
                        ▼  [session_try_acquire]
             ┌──────────────────────┐
             │  Decentralized Loop  │ ◄─── Execution Thread (Any)
             └──────────┬───────────┘
                        │
                        ▼  [BEGIN EXCLUSIVE]
            inbox_consume_into_entries()
                        │
                        ▼
                  agent_run()

```

### Core Invariants

* **V16:** Every execution of `agent_run()` requires the target session to be in the `'running'` state, explicitly bound to a unique thread/process identifier via a Compare-And-Swap (CAS) lock.
* **V17:** Conversation steps are grouped into logical `turn_id` segments. If a turn is interrupted (e.g., process crash mid-tool execution), it is caught during `context_build()` and structurally neutralized.
* **V18:** Inbox consumption and entry production must happen inside a single SQLite transaction, ensuring messages are neither lost nor duplicated.
* **V19:** All session state updates must pass through strict SQL clauses targeting expected current states to prevent Time-of-Check to Time-of-Use (TOCTOU) race conditions.

---

## 2. Storage Engine Schema Migrations

```sql
-- Migration Script: V14_Robustness_And_Inbox.sql
BEGIN TRANSACTION;

-- Session State Machine Extensions
ALTER TABLE sessions ADD COLUMN state TEXT NOT NULL DEFAULT 'idle';
ALTER TABLE sessions ADD COLUMN locked_by TEXT;
ALTER TABLE sessions ADD COLUMN locked_at INTEGER;
ALTER TABLE sessions ADD COLUMN error_count INTEGER NOT NULL DEFAULT 0;

-- Turn Identification Extensions
ALTER TABLE entries ADD COLUMN turn_id INTEGER;

-- Composite Index for O(1) Max Turn ID Lookups via Index-Only Scans
CREATE INDEX IF NOT EXISTS idx_entries_session_turn 
  ON entries(session_id, turn_id);

-- Durable Transactional Inbox Queue
CREATE TABLE IF NOT EXISTS inbox (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id INTEGER NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
    source TEXT NOT NULL,
    content TEXT NOT NULL,
    priority INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL DEFAULT (unixepoch()),
    consumed_at INTEGER DEFAULT NULL
);

-- Partial Index for Active Inbox Scanning
CREATE INDEX IF NOT EXISTS idx_inbox_pending
  ON inbox(session_id) WHERE consumed_at IS NULL;

-- Sub-Agent Delivery Directives
ALTER TABLE sub_agents ADD COLUMN on_complete TEXT NOT NULL DEFAULT 'inbox';

COMMIT;

```

---

## 3. Session State Machine & CAS Primitives

Sessions transition through four operational states:

```
        ┌─────────────────── inbox_insert() ──────────────────┐
        ▼                                                     │
    ┌───────┐             ┌─────────┐  runner picks up  ┌─────────┐
    │ idle  │ ──────────► │ pending │ ────────────────► │ running │
    └───────┘             └─────────┘                   └────┬────┘
        │                      ▲                             │
        │  direct agent_run()  │                             │ crash / stale lock
        └──────────────────────┼─────────────────────────────┼────────┐
                               │                             ▼        ▼
                               └───────── janitor ────── ┌───────┐ ┌───────┐
                                          error_count <3 │ error │ │ fatal │
                                                         └───────┘ └───────┘

```

### Atomic CAS Acquisition

```c
int session_try_acquire(sqlite3 *db, int64_t session_id, const char *owner) {
    const char *sql =
        "UPDATE sessions "
        "SET state='running', locked_by=?, locked_at=unixepoch() "
        "WHERE id=? AND state IN ('idle', 'pending');";
        
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return 0;
    
    sqlite3_bind_text(stmt, 1, owner, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, session_id);
    
    rc = sqlite3_step(stmt);
    int changes = sqlite3_changes(db);
    sqlite3_finalize(stmt);
    
    return (rc == SQLITE_DONE && changes == 1);
}

```

---

## 4. The Keep-Alive Release Architecture

To eliminate edge-case timing windows where a message hits the inbox during the wrap-up of a turn, the processing thread executes a structural release phase. It leverages a strict `BEGIN IMMEDIATE` boundary to block incoming concurrent modifiers until the state resolution settles.

```c
#define MAX_CONSECUTIVE_TURNS_DEFAULT 3

void session_release_loop(sqlite3 *db, int64_t session_id, const char *caller_id, int max_turns, AgentContext *ctx) {
    int turns_executed = 0;
    
    while (1) {
        // Derive next monotonic turn ID
        int64_t turn_id = db_get_next_turn_id(db, session_id);
        
        // 1. Consume current inbox payload into active session entries
        int consumed = inbox_consume_into_entries(db, session_id, turn_id);
        
        // 2. Run the LLM Loop iteration if entries exist or if initialized directly
        if (consumed > 0 || turns_executed == 0) {
            agent_run(ctx, turn_id);
            turns_executed++;
        }
        
        // 3. Enter Critical Section for State Determination
        sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);
        
        int pending_msgs = db_get_unconsumed_inbox_count(db, session_id);
        
        if (pending_msgs == 0 || turns_executed >= max_turns) {
            const char *target_state = (pending_msgs > 0) ? "pending" : "idle";
            
            sqlite3_stmt *stmt;
            const char *sql = "UPDATE sessions SET state=?, locked_by=NULL, locked_at=NULL, error_count=0 "
                              "WHERE id=? AND state='running';";
            
            sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
            sqlite3_bind_text(stmt, 1, target_state, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 2, session_id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            
            sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
            break; 
        } else {
            // New items accumulated during agent_run. Retain lock, commit transaction, loop back.
            sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
            continue;
        }
    }
}

```

---

## 5. Fault Detection & Fault Isolation (The Janitor Daemon)

The background main loop intercepts orphan states via two mechanics: **Stale Lock Mitigation** and a hard **Quarantine Boundary** for deterministic execution crashes.

### The Janitor Sweep

```c
void janitor_sweep(sqlite3 *db, int lock_timeout_seconds) {
    // Phase 1: Catch crashed running sessions and flag errors
    const char *stale_sql = 
        "UPDATE sessions SET state='error', locked_by=NULL, locked_at=NULL, error_count = error_count + 1 "
        "WHERE state='running' AND locked_at < (unixepoch() - ?);";
        
    sqlite3_stmt *stmt1;
    sqlite3_prepare_v2(db, stale_sql, -1, &stmt1, NULL);
    sqlite3_bind_int(stmt1, 1, lock_timeout_seconds);
    sqlite3_step(stmt1);
    sqlite3_finalize(stmt1);

    // Phase 2: Auto-recover unblocked sessions under error thresholds
    // If pending work exists and error_count < 3, flag to 'pending' for retry
    const char *recover_pending = 
        "UPDATE sessions SET state='pending', error_count = error_count + 1 "
        "WHERE state='error' AND error_count < 3 "
        "AND EXISTS (SELECT 1 FROM inbox WHERE session_id = sessions.id AND consumed_at IS NULL);";
    sqlite3_exec(db, recover_pending, NULL, NULL, NULL);

    // Phase 3: Move error states with no pending entries cleanly back to idle
    const char *recover_idle = 
        "UPDATE sessions SET state='idle' "
        "WHERE state='error' AND error_count < 3 "
        "AND NOT EXISTS (SELECT 1 FROM inbox WHERE session_id = sessions.id AND consumed_at IS NULL);";
    sqlite3_exec(db, recover_idle, NULL, NULL, NULL);
}

```

### Structural Incomplete Turn Interception

When `context_build()` evaluates the historical entries table, it scans the tail turn sequence.

If an assistant role entry declares a `tool_calls` payload, but the matching execution elements are missing due to a crash, the engine **must not skip the turn** (which causes severe side-effect blindspots and breaks vendor schema validation APIs).

The builder instead builds virtual structural objects:

1. Synthesizes synthetic `tool` response entries containing generic failure strings (`{"error": "Process terminated during execution"}`) to satisfy schema structural balance rules.
2. Injects a system explicit notice turn immediately following:
> `[Previous turn was interrupted. Tool call results may be missing. You may retry the operation if needed.]`



---

## 6. Implementation Specification Tasks (§T)

```
        PHASE A: ROBUSTNESS CORE          PHASE B: INBOX PIPELINES
        ├── T55: DB Schema Migrations     ├── T62: inbox Core Primitives
        ├── T56: CAS Acquire/Release      ├── T63: Atomic Move Transaction
        ├── T57: agent.c Turn Tagging     ├── T64: Subagent Wake Injection
        ├── T58: context.c Incomplete     ├── T65: spawn_agent API Updates
        ├── T59: Janitor Sweep Logic      └── T66: Verification Tests
        ├── T60: Anti-Crash Loop Limit
        └── T61: Phase A Integration

```

### Phase A: Session Robustness Core

* **T55:** Execute schema modifications adding state flags, lock definitions, and error analytics structures to `sessions` and `entries`.
* **T56:** Implement `session_try_acquire` and the structural state verification queries within `src/db.c`.
* **T57:** Refactor `src/agent.c` processing chains to compute next sequential increments for `turn_id` utilizing index-optimized queries.
* **T58:** Implement validation passes within `src/context.c` to look for asymmetric tool outputs, outputting synthetic validation frames and system notifications.
* **T59:** Add `janitor_sweep()` to the main daemon execution block (`src/main.c`) to query stale locks on a 1-second cadence.
* **T60:** Construct state constraints within janitor loops isolating systems matching or exceeding `MAX_AUTO_RECOVERY` criteria.
* **T61:** Establish verification matrices proving execution rejection patterns across overlapping workers.

### Phase B: Transactional Inbox Pipelines

* **T62:** Build out `inbox` table management utilities (`inbox_insert`, `inbox_peek`) within storage files.
* **T63:** Implement `inbox_consume_into_entries()` wrapped explicitly in an isolated transactional block (`BEGIN EXCLUSIVE`).
* **T64:** Patch sub-agent lifecycle terminations in `src/tool_subagent.c` to feed outcomes directly into parent inbox descriptors rather than relying on manual poll iterations.
* **T65:** Extend the `spawn_agent` tool specification parameters to recognize and route configuration keys (`inbox`, `silent`, `spawn`).
* **T66:** Author test components measuring atomic rollbacks during simulated operational failures mid-consumption step.

### Phase C: Decentralized Integration

* **T67:** Upgrade `src/cli.c` workspace triggers to conform to the safe acquire/release keep-alive framework.
* **T68:** Transition `src/telegram.c` intake handlers to write text bodies to the durable inbox queue and instantly trigger local lock attempts.
* **T69:** Update `src/cron.c` process actions to execute within standard transactional inbox wrappers.
* **T70:** Verify that parallel high-throughput telemetry streams stay ordered across overlapping network workloads.

### Phase D: Polish & Observability

* **T71:** Expose state metrics, lock holders, and backlog queue depths on the web monitoring console (`src/web.c`).
* **T72:** Enhance CLI terminal resumption paths to cleanly echo unread inbox counts when opening interactive prompt tracking sessions.
* **T73:** Add the `inbox_list` runtime introspection tool to the core system registry.
* **T74:** Bind internal parameters (such as `stale_lock_timeout`) directly to unified runtime config configuration nodes.