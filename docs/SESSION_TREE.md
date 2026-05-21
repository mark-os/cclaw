# Session Tree Design

## Overview

CClaw sessions are stored as a tree of entries. Phase 1 operates as a simple unidirectional stack (append-only, delete/summarize from the end). The schema is designed so that branching (fork, navigate, branch summaries) can be added later without migration.

## Core Concept

Every piece of session state is an **entry** with an `id` and a `parent_id`. The current conversation is reconstructed by walking `parent_id` from the **leaf** back to the root. This is Pi's model.

```
root ← entry1 ← entry2 ← entry3 ← entry4 (leaf)
```

In Phase 1, this is just a linked list. In the future, forking creates a second chain from any entry:

```
root ← e1 ← e2 ← e3 ← e4 (branch A leaf)
                   └── e5 ← e6 (branch B leaf)
```

## SQLite Schema

```sql
CREATE TABLE sessions (
    id          TEXT PRIMARY KEY,
    created_at  TEXT NOT NULL,
    name        TEXT
);

CREATE TABLE entries (
    id          TEXT PRIMARY KEY,
    session_id  TEXT NOT NULL REFERENCES sessions(id),
    parent_id   TEXT REFERENCES entries(id),  -- NULL = root
    type        TEXT NOT NULL,                 -- 'message', 'compaction', 'model_change', etc.
    timestamp   TEXT NOT NULL,
    payload     TEXT NOT NULL                  -- JSON blob, schema depends on type
);

CREATE TABLE session_state (
    session_id  TEXT PRIMARY KEY REFERENCES sessions(id),
    leaf_id     TEXT REFERENCES entries(id)    -- current active leaf
);

CREATE INDEX idx_entries_session ON entries(session_id);
CREATE INDEX idx_entries_parent ON entries(parent_id);
```

## Entry Types

### message
```json
{
  "role": "user|assistant|tool_result",
  "content": "text content",
  "tool_calls": [{"id": "...", "name": "...", "arguments": "..."}],
  "tool_call_id": "...",
  "model": "deepseek-v4-flash",
  "provider": "nvidia",
  "stop_reason": "stop|tool_calls|length|error",
  "usage": {"input": 100, "output": 50}
}
```

### compaction
```json
{
  "summary": "Summary of compacted messages...",
  "first_kept_entry_id": "entry-id",
  "tokens_before": 45000
}
```

### model_change
```json
{
  "provider": "nvidia",
  "model_id": "deepseek-v4-flash"
}
```

## Data Flow

The in-memory message array is the source of truth during a live session. JSON only exists at boundaries:

```
                    ┌─────────────────────────────┐
                    │  In-Memory: Message structs  │
                    │  (C arrays, no JSON)         │
                    └──────┬──────────────┬───────┘
                           │              │
              serialize    │              │  serialize
              (cJSON)      │              │  (cJSON)
                           ▼              ▼
                    ┌────────────┐  ┌───────────┐
                    │ LLM API    │  │  SQLite   │
                    │ (JSON over │  │  (JSON in │
                    │  HTTP)     │  │  payload) │
                    └────────────┘  └───────────┘
```

- **User message arrives** → append C struct to array (no JSON involved)
- **Call LLM** → walk array, build cJSON request, serialize, POST
- **Parse response** → cJSON parse, extract into C struct, append to array, free cJSON
- **Tool execution** → read args from struct, execute, append result struct
- **Persist** → serialize struct to JSON string, INSERT into SQLite
- **Load session** → SELECT from SQLite, parse JSON payloads into C structs, populate array

No deserialization during a conversation. No double representation. JSON is transient at I/O boundaries only.

## Phase 1 Operations

Phase 1 treats the session as a stack. Only these operations are supported:

- **append(entry)** — add entry with `parent_id = current leaf`, update leaf
- **get_branch()** — walk from leaf to root, return entries in chronological order
- **delete_from_end(n)** — remove last n entries, update leaf to new tail
- **summarize_and_trim(n)** — summarize last n entries into a compaction entry, remove them, append compaction

### In-Memory Representation (Phase 1)

During a session, the branch is loaded into a flat array in memory:

```c
typedef struct {
    char *id;
    char *parent_id;
    EntryType type;
    char *payload_json;  // raw JSON, parsed on demand
    int64_t timestamp;
} Entry;

typedef struct {
    char *session_id;
    Entry *entries;      // dynamic array, chronological order
    int entry_count;
    int entry_capacity;
    char *leaf_id;
} SessionBranch;
```

The array is the working set. SQLite is the persistence layer. On startup, load the branch. On each turn, append entries and flush to SQLite.

## Future: Branching Operations (Phase 2+)

These will be added later but the schema already supports them:

- **fork(entry_id)** — set leaf to entry_id, future appends create a new branch
- **navigate(entry_id)** — switch leaf, optionally generate branch summary
- **list_branches(session_id)** — find all leaf entries (entries with no children)
- **branch_summary** — when navigating away, summarize the abandoned branch

## Design Decisions

1. **IDs are UUIDs** — simple, no coordination needed for sub-agents
2. **Payload is JSON** — flexible, no schema migrations when adding entry types
3. **Leaf is explicit** — stored in session_state, not computed (avoids scanning)
4. **One session per conversation** — Telegram chat_id maps to session_id
5. **Entries are immutable** — never update, only append (except delete-from-end in Phase 1)
6. **Compaction replaces messages** — old entries are deleted, compaction entry summarizes them

## Relationship to Sub-Agents

Each sub-agent gets its own session. Parent-child relationship tracked via:

```sql
ALTER TABLE sessions ADD COLUMN parent_session_id TEXT REFERENCES sessions(id);
ALTER TABLE sessions ADD COLUMN spawn_depth INTEGER DEFAULT 0;
```

This is Phase 2+ work. The schema is ready for it.
