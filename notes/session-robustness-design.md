# Technical Design Document: Session Robustness & Transactional Inbox Architecture

---

## 1. System Architecture Overview

The CClaw concurrency and fault-tolerance model is a **decentralized, state-machine-driven transactional system** powered by SQLite primitives. No central runner thread — any thread/process that needs to run a session acquires it via CAS.

```
   [Telegram Thread]      [Cron Thread]     [Sub-Agent Process]
          │                     │                     │
          ▼                     ▼                     ▼
    inbox_insert()        inbox_insert()        inbox_insert()
          │                     │                     │
          └──────────────┬──────┴─────────────────────┘
                         ▼
                ┌────────────────┐
                │  SQLite INBOX  │ ◄─── Durable Queue (passive, no wake triggers)
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

## 2. Schema

```sql
-- Sessions with CAS lock fields
CREATE TABLE sessions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT,
    leaf_id INTEGER DEFAULT -1,
    agent_name TEXT,
    state TEXT NOT NULL DEFAULT 'idle',
    lock_holder TEXT,
    lock_acquired_at INTEGER,
    error_count INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL DEFAULT (unixepoch()),
    updated_at INTEGER NOT NULL DEFAULT (unixepoch())
);

-- Entries: JSON data column with generated columns for indexing
CREATE TABLE entries (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id INTEGER NOT NULL REFERENCES sessions(id),
    parent_id INTEGER NOT NULL DEFAULT -1,
    turn_id INTEGER,
    created_at INTEGER NOT NULL DEFAULT (unixepoch()),
    data TEXT NOT NULL,
    type TEXT GENERATED ALWAYS AS (json_extract(data, '$.type')) STORED,
    role TEXT GENERATED ALWAYS AS (json_extract(data, '$.role')) STORED
);

CREATE INDEX idx_entries_session ON entries(session_id, id);
CREATE INDEX idx_entries_parent ON entries(parent_id);
CREATE INDEX idx_entries_session_type ON entries(session_id, type);
CREATE INDEX idx_entries_session_role ON entries(session_id, role);
CREATE INDEX idx_entries_turn ON entries(session_id, turn_id);

-- Durable Transactional Inbox Queue
CREATE TABLE inbox (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id INTEGER NOT NULL REFERENCES sessions(id),
    source TEXT NOT NULL,
    payload TEXT NOT NULL,
    created_at INTEGER NOT NULL DEFAULT (unixepoch()),
    consumed INTEGER NOT NULL DEFAULT 0
);

CREATE INDEX idx_inbox_pending ON inbox(session_id, consumed) WHERE consumed = 0;
```

---

## 3. Session State Machine & CAS Primitives

Sessions transition through these states:

```
    ┌───────┐             ┌─────────┐
    │ idle  │ ──────────► │ running │
    └───────┘  acquire    └────┬────┘
        ▲                      │
        │  release (clean)     │ crash / stale lock
        │                      ▼
        │                 ┌─────────┐  error_count >= 3  ┌────────────┐
        └──── janitor ─── │  error  │ ─────────────────► │ quarantine │
              (retry)     └─────────┘                    └────────────┘
```

States:
- **idle** — no work pending, no lock held
- **pending** — inbox has items but no thread is running the session (set by release loop when max_turns hit)
- **running** — lock held, agent executing
- **error** — crashed mid-turn, janitor detected stale lock
- **quarantine** — error_count >= 3, requires manual intervention

### Atomic CAS Acquisition

```c
int session_try_acquire(sqlite3 *db, int64_t session_id, const char *owner) {
    const char *sql =
        "UPDATE sessions "
        "SET state='running', lock_holder=?, lock_acquired_at=unixepoch() "
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

## 4. The Keep-Alive Release Loop

After agent_run completes a turn, the thread checks if new inbox items arrived during execution. If so, it drains them and runs another turn — up to a max. This avoids releasing the lock only to immediately re-acquire it.

```c
#define MAX_CONSECUTIVE_TURNS 3

void session_run_loop(sqlite3 *db, int64_t session_id, const char *owner, AgentContext *ctx) {
    int turns_executed = 0;

    while (1) {
        int64_t turn_id = db_get_next_turn_id(db, session_id);

        // Consume inbox into entries
        int consumed = inbox_consume_into_entries(db, session_id, turn_id);

        // Run agent if there's work
        if (consumed > 0 || turns_executed == 0) {
            agent_run(ctx, turn_id);
            turns_executed++;
        }

        // Critical section: decide whether to release or continue
        sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL);

        int pending = db_get_unconsumed_inbox_count(db, session_id);

        if (pending == 0 || turns_executed >= MAX_CONSECUTIVE_TURNS) {
            const char *target_state = (pending > 0) ? "pending" : "idle";

            sqlite3_stmt *stmt;
            const char *sql =
                "UPDATE sessions SET state=?, lock_holder=NULL, lock_acquired_at=NULL, error_count=0 "
                "WHERE id=? AND state='running';";

            sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
            sqlite3_bind_text(stmt, 1, target_state, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 2, session_id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);

            sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
            break;
        } else {
            // More items arrived during agent_run — loop back
            sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
            continue;
        }
    }
}
```

---

## 5. Fault Detection & Isolation (Janitor)

The janitor runs in the daemon's main loop on a periodic cadence (e.g., every 30s). It detects crashed sessions via stale locks and either retries or quarantines them.

```c
void janitor_sweep(sqlite3 *db, int lock_timeout_seconds) {
    // Phase 1: Detect stale locks → mark as error
    const char *stale_sql =
        "UPDATE sessions SET state='error', lock_holder=NULL, lock_acquired_at=NULL, "
        "error_count = error_count + 1 "
        "WHERE state='running' AND lock_acquired_at < (unixepoch() - ?);";

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, stale_sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, lock_timeout_seconds);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Phase 2: Recover error sessions with pending inbox (under threshold)
    const char *recover_sql =
        "UPDATE sessions SET state='pending' "
        "WHERE state='error' AND error_count < 3 "
        "AND EXISTS (SELECT 1 FROM inbox WHERE session_id = sessions.id AND consumed = 0);";
    sqlite3_exec(db, recover_sql, NULL, NULL, NULL);

    // Phase 3: Error sessions with no pending work → idle
    const char *idle_sql =
        "UPDATE sessions SET state='idle' "
        "WHERE state='error' AND error_count < 3 "
        "AND NOT EXISTS (SELECT 1 FROM inbox WHERE session_id = sessions.id AND consumed = 0);";
    sqlite3_exec(db, idle_sql, NULL, NULL, NULL);
}
```

---

## 6. Incomplete Turn Interception

When `context_build()` loads the branch, it checks the tail turn for structural completeness.

If an assistant entry has tool_calls in its `data.content` array but the corresponding `tool_result` entries are missing (crash mid-execution), the builder:

1. Synthesizes tool_result entries with error content: `{"type":"message","role":"tool_result","tool_call_id":"...","name":"...","content":"error: process terminated during execution","is_error":true}`
2. Injects a system notice: `[Previous turn was interrupted. Tool results may be incomplete. Retry if needed.]`

This satisfies the LLM's schema requirements (every tool_call must have a matching tool_result) without skipping the turn or losing context about what was attempted.

---

## 7. Sub-Agent Result Delivery

Sub-agents use two modes (decided at spawn time by the parent):

- **Blocking** (default): Parent thread waits on child process. Result returned directly as the `spawn_agent` tool output. No inbox involved.
- **Background**: Parent continues its turn. Sub-agent runs independently. On completion, sub-agent posts its name+id to the parent's inbox. Parent sees it on next wake and can use `db_query` to read the sub-agent's session entries.

The inbox is **passive storage** — producers never trigger wakes. A session only runs when an external event (user message, Telegram, cron) acquires its lock. Inbox is drained at the start of each run via `inbox_consume_into_entries()`.
