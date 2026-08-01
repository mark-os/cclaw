# Memory Model

Design reference. Derived from Letta's persistent agent memory architecture, adapted for CClaw.

## Three Tiers

| Tier | CClaw Implementation | Access Pattern |
|------|---------------------|----------------|
| Core (in-context) | `memory_blocks` table in cclaw.db (scoped by `agent_name`) | Always in context, bounded by `char_limit`; `placement` picks system prompt or per-turn context block |
| Archival (long-term) | Not yet implemented: `passages` table + embeddings | Semantic search, unlimited |
| Recall (history) | `entries` table + FTS5 in cclaw.db | Keyword/semantic search over all past turns |

## Core Memory (memory_blocks)

Agent's working memory — always in context. Lives in cclaw.db, scoped by `agent_name` column.

Where a block renders is its `placement`:
- `context` (default for new blocks): rendered into the `<RELEVANT_CONTEXT>` block (`llm_payload.c`), which rides at the **turn boundary** — a user message right before the newest user_message, never in the system prompt. The block (local wall clock with its timezone + recall + context-placement blocks + running sub-agents + pending approvals) is **materialized once at turn start** (`sessions.turn_context`, `llm_proc.c`) and reused verbatim by every tool-loop iteration, so both its content and position stay stable for the whole turn and each iteration extends the prompt-cache prefix instead of invalidating it. Live state is thus a turn-start snapshot; mid-turn changes reach the model as tool results.
- `system`: baked into the system prompt once per session (`agent_config.c`). Reserve for stable identity content — the system prompt stays byte-identical across turns, so provider prompt caching keeps working, but every edit invalidates the whole cached prefix on the next session render. The base Assistant's seeded `AGENT` and `USER` blocks use this placement.

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
    placement TEXT NOT NULL DEFAULT 'system',  -- 'system' | 'context'
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
- Auto-recall (implemented, FTS5-only today): top hits ride in the `<RELEVANT_CONTEXT>` block at the turn boundary — see Core Memory above

## Context Window Management

- Load ≤ `context_threshold` × context_window tokens of most recent turns
- Never cut mid-tool-call; the cut snaps **backwards to the start of the turn it
  lands in** (`entries.turn_id`, `plan_find_cut` in `context.c`) — not to "the
  previous user message", which would split a turn that opened with several
  inbox-drained user entries, and not forwards, which could drop the very turn
  being answered. Keeping a whole turn can overshoot the budget by one turn.
- Cutoff notice prepended when truncated
- Compaction: summarize old entries, reparent tree. Its keep boundary
  (`context_compaction_keep_from`) snaps the same way, so a summary never lands
  inside a turn; a branch that is one giant turn compacts to nothing.

## Letta Patterns NOT Adopted

| Letta Feature | CClaw Decision | Reason |
|---------------|---------------|--------|
| Shared blocks across agents | Implemented (`agent_name IS NULL` = global block) | — |
| Sleeptime background agent | Not implemented | Adds complexity; agent can manage own memory |
| Stateless mode | Not needed | Sessions are cheap, tree structure handles branching |
| PostgreSQL + pgvector | SQLite single-file | No server dependency, trivial deployment |
| 100K char block limit | 5K default | Smaller context windows, cheaper models |
