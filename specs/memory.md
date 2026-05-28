# Memory Model

Design reference. Derived from Letta's persistent agent memory architecture, adapted for CClaw.

## Three Tiers

| Tier | CClaw Implementation | Access Pattern |
|------|---------------------|----------------|
| Core (in-context) | `memory_blocks` table in agent.db | Always in system prompt, bounded by `char_limit` |
| Archival (long-term) | §F: `passages` table + embeddings | Semantic search, unlimited |
| Recall (history) | `entries` table + FTS5 in agent.db | Keyword/semantic search over all past turns |

## Core Memory (memory_blocks)

Agent's working memory — injected into system prompt every turn. Lives in per-agent DB (`agents/<name>/agent.db`).

- **Unit**: labeled block with character limit
- **Default blocks**: created by agent via `memory_create` tool (or pre-seeded by daemon at agent creation)
- **Agent-editable**: via `memory_append`, `memory_replace` tools
- **Read-only option**: `read_only=1` → visible but agent tools reject edits
- **Scoped per-agent**: not per-session (persists across all sessions for that agent)
- **Rendered in prompt**: with metadata (label, description, chars_used/limit)
- **No `agent_name` column**: each agent has own DB, so blocks are implicitly agent-scoped

Key insight from Letta: memory management is a **tool-use problem**. The agent decides what to remember via tool calls, not system-level heuristics.

## Schema (in agent.db)

```sql
CREATE TABLE memory_blocks (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    label TEXT NOT NULL UNIQUE,
    value TEXT NOT NULL DEFAULT '',
    description TEXT,
    char_limit INTEGER NOT NULL DEFAULT 5000,
    read_only INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL DEFAULT (unixepoch()),
    updated_at INTEGER NOT NULL DEFAULT (unixepoch())
);
```

## Recall (entries + FTS5)

All past messages searchable via `db_query` tool (read-only SQL) or future auto-recall.

- FTS5 indexes `content` column directly (split-column schema)
- Agent can search own history: `SELECT content FROM entries_fts WHERE entries_fts MATCH ?`
- Context window manager (V7) loads recent turns; older entries accessible via search
- All in agent.db — agent has full RW access

## Future: Archival (vector recall)

From §F FUTURE:
- `passages` table (text, embedding BLOB, source_type, source_id) — in agent.db
- Hybrid search: FTS5 + vector similarity (RRF merge)
- Agent tool: `memory_search(query)` for explicit recall
- Auto-recall: inject top hits into system prompt at context build time

## Context Window Management

- V7: load ≤ `max_history_tokens` (60% of context window) most recent turns
- V8: never cut mid-tool-call; cut at valid turn boundary
- Cutoff notice prepended when truncated
- Compaction (T160-T163): summarize old entries, reparent tree

## Letta Patterns NOT Adopted

| Letta Feature | CClaw Decision | Reason |
|---------------|---------------|--------|
| Shared blocks across agents | Not implemented | Single-user system, per-agent DB isolation |
| Sleeptime background agent | Not implemented | Adds complexity; agent can manage own memory |
| Stateless mode | Not needed | Sessions are cheap, tree structure handles branching |
| PostgreSQL + pgvector | SQLite per-agent | Single-file DB, no server dependency |
| 100K char block limit | 5K default | Smaller context windows, cheaper models |
