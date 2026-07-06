# Memory Model

Design reference. Derived from Letta's persistent agent memory architecture, adapted for CClaw.

## Three Tiers

| Tier | CClaw Implementation | Access Pattern |
|------|---------------------|----------------|
| Core (in-context) | `memory_blocks` table in cclaw.db (scoped by `agent_name`) | Always in system prompt, bounded by `char_limit` |
| Archival (long-term) | Not yet implemented: `passages` table + embeddings | Semantic search, unlimited |
| Recall (history) | `entries` table + FTS5 in cclaw.db | Keyword/semantic search over all past turns |

## Core Memory (memory_blocks)

Agent's working memory — injected into system prompt every turn. Lives in cclaw.db, scoped by `agent_name` column.

- **Unit**: labeled block with character limit
- **Default blocks**: created by agent via `memory_create` tool (or pre-seeded by daemon at agent creation)
- **Agent-editable**: via `memory_add`, `memory_edit`, `memory_delete` tools
- **Read-only option**: `read_only=1` → visible but agent tools reject edits
- **Scoped per-agent**: `agent_name` column isolates blocks; persists across all sessions for that agent. `NULL` = global block, visible to every agent.
- **Rendered in prompt**: with metadata (label, description, chars_used/limit)

Key insight from Letta: memory management is a **tool-use problem**. The agent decides what to remember via tool calls, not system-level heuristics.

## Schema (in cclaw.db)

```sql
CREATE TABLE memory_blocks (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    agent_name TEXT,                    -- NULL = global (shared across agents)
    label TEXT NOT NULL,
    value TEXT NOT NULL DEFAULT '',
    description TEXT,
    char_limit INTEGER NOT NULL DEFAULT 5000,
    read_only INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL DEFAULT (unixepoch()),
    updated_at INTEGER NOT NULL DEFAULT (unixepoch()),
    UNIQUE(agent_name, label)
);
```

## Recall (entries + FTS5)

All past messages searchable via `db_query` tool (read-only SQL) or auto-recall.

- FTS5 indexes `content` column directly (split-column schema)
- Agent can search own history: `SELECT content FROM entries_fts WHERE entries_fts MATCH ?`
- Context window manager loads recent turns; older entries accessible via search
- All in cclaw.db — agent reads via the `db_query` tool, not a direct file path

## Future: Archival (vector recall)

Not yet implemented:
- `passages` table (text, embedding BLOB, source_type, source_id) — in cclaw.db
- Hybrid search: FTS5 + vector similarity (RRF merge)
- Agent tool: `memory_search(query)` for explicit recall
- Auto-recall: inject top hits into system prompt at context build time

## Context Window Management

- Load ≤ `context_threshold` × context_window tokens of most recent turns
- Never cut mid-tool-call; cut at valid turn boundary
- Cutoff notice prepended when truncated
- Compaction: summarize old entries, reparent tree

## Letta Patterns NOT Adopted

| Letta Feature | CClaw Decision | Reason |
|---------------|---------------|--------|
| Shared blocks across agents | Implemented (`agent_name IS NULL` = global block) | — |
| Sleeptime background agent | Not implemented | Adds complexity; agent can manage own memory |
| Stateless mode | Not needed | Sessions are cheap, tree structure handles branching |
| PostgreSQL + pgvector | SQLite single-file | No server dependency, trivial deployment |
| 100K char block limit | 5K default | Smaller context windows, cheaper models |
