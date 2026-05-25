# CClaw — DB Schema

SQLite 3.53, WAL mode, `busy_timeout=5000`. Sole persistence layer.

## Tables

```sql
CREATE TABLE sessions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT,
    agent_name TEXT,                          -- V20: identifies agent config (agents/<name>/)
    parent_session_id INTEGER DEFAULT -1,     -- sub-agent parent (-1 = top-level)
    depth INTEGER NOT NULL DEFAULT 0,         -- sub-agent depth (0 = top-level)
    leaf_id INTEGER DEFAULT -1,
    state TEXT NOT NULL DEFAULT 'idle',       -- idle|running|waiting
    lock_holder TEXT,                         -- "cli-PID" (CLI CAS); daemon uses fork-exclusivity
    lock_acquired_at INTEGER,
    error_count INTEGER NOT NULL DEFAULT 0,
    last_route TEXT,                          -- V26: "telegram:<chat_id>", "cli", "cron:<job>", "subagent:<sid>"
    created_at INTEGER NOT NULL DEFAULT (unixepoch()),
    updated_at INTEGER NOT NULL DEFAULT (unixepoch())
);

CREATE TABLE entries (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id INTEGER NOT NULL REFERENCES sessions(id),
    parent_id INTEGER NOT NULL DEFAULT -1,
    turn_id INTEGER,
    created_at INTEGER NOT NULL DEFAULT (unixepoch()),
    data TEXT NOT NULL,                       -- JSON payload (see §D in SPEC.md)
    -- Generated columns for indexing without JSON parsing
    type TEXT GENERATED ALWAYS AS (json_extract(data, '$.type')) STORED,
    role TEXT GENERATED ALWAYS AS (json_extract(data, '$.role')) STORED,
    stop_reason TEXT GENERATED ALWAYS AS (json_extract(data, '$.stop_reason')) STORED
);

CREATE TABLE inbox (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id INTEGER NOT NULL REFERENCES sessions(id),
    source TEXT NOT NULL,                     -- "telegram:<chat_id>", "cli", "cron:<job>", "subagent:<sid>"
    payload TEXT NOT NULL,
    created_at INTEGER NOT NULL DEFAULT (unixepoch()),
    consumed INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE spawn_queue (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    parent_session_id INTEGER NOT NULL REFERENCES sessions(id),
    agent_name TEXT NOT NULL,
    task TEXT NOT NULL,
    mode TEXT NOT NULL DEFAULT 'blocking',    -- blocking|background
    tool_call_id TEXT,                        -- for blocking: match result to parent's tool_call
    created_at INTEGER NOT NULL DEFAULT (unixepoch()),
    consumed INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE cron_jobs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL,
    cron_expr TEXT NOT NULL,
    session_id INTEGER NOT NULL REFERENCES sessions(id),
    task TEXT NOT NULL,
    enabled INTEGER NOT NULL DEFAULT 1,
    next_run_at INTEGER NOT NULL DEFAULT 0,
    last_run_at INTEGER,
    created_at INTEGER NOT NULL DEFAULT (unixepoch())
);

CREATE TABLE js_tools (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session_id INTEGER NOT NULL REFERENCES sessions(id),
    name TEXT NOT NULL,
    description TEXT,
    parameters_json TEXT,
    code TEXT NOT NULL,
    UNIQUE(session_id, name)
);

CREATE TABLE tg_chat_sessions (
    chat_id INTEGER PRIMARY KEY,
    session_id INTEGER NOT NULL REFERENCES sessions(id)
);

CREATE TABLE kv (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
```

## Indexes

```sql
CREATE INDEX idx_entries_session ON entries(session_id, id);
CREATE INDEX idx_entries_parent ON entries(parent_id);
CREATE INDEX idx_entries_session_type ON entries(session_id, type);
CREATE INDEX idx_entries_session_role ON entries(session_id, role);
CREATE INDEX idx_entries_turn ON entries(session_id, turn_id);
CREATE INDEX idx_entries_stop_reason ON entries(session_id, stop_reason) WHERE stop_reason IS NOT NULL;
CREATE INDEX idx_inbox_pending ON inbox(session_id, consumed) WHERE consumed = 0;
CREATE INDEX idx_spawn_queue_pending ON spawn_queue(consumed) WHERE consumed = 0;
```

## FTS5

```sql
CREATE VIRTUAL TABLE entries_fts USING fts5(content, content=entries, content_rowid=id);
-- AFTER INSERT trigger extracts searchable text from JSON data
```

## Entry `data` JSON Formats

```
message/user:        {"type":"message","role":"user","content":"..."}
message/assistant:   {"type":"message","role":"assistant","content":[...],"model":"...","usage":{"input":N,"output":N},"stop_reason":"stop|length|tool_use|error|aborted"}
message/tool_result: {"type":"message","role":"tool_result","tool_call_id":"...","name":"...","content":"...","is_error":false}
message/system:      {"type":"message","role":"system","content":"..."}
compaction:          {"type":"compaction","summary":"...","first_kept_id":N}
```

## Design Decisions

1. **JSON in `data`** — flexible schema evolution without migrations. Generated columns give indexed access without parsing at query time.
2. **INTEGER ids** — faster joins, smaller indexes, natural ordering. No UUIDs (single-writer per session).
3. **parent_id tree** — supports branching (Pi model). Walk leaf→root for current branch.
4. **Inbox as table** — durable queue. Survives crashes. Atomic consumption via `BEGIN EXCLUSIVE`.
5. **WAL mode** — multiple readers never block. Writers serialize briefly on commit.
6. **`stop_reason` generated column** — enables V36 filtering without JSON parsing at query time.
7. **`spawn_queue`** — daemon reads on signal pipe wake, forks sub-agent, marks consumed. Decouples agent process from fork logic (V21).
8. **Sub-agents are sessions** — no separate tracking table. `parent_session_id` + `depth` on sessions table. V3 limits enforced via `SELECT COUNT(*) FROM sessions WHERE parent_session_id=? AND state IN ('running','waiting')`. Result read from child session's entries.
