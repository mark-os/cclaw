# Memory Model

Design reference. Derived from Letta's persistent agent memory architecture, adapted for CClaw.

## Three Tiers

| Tier | CClaw Implementation | Access Pattern |
|------|---------------------|----------------|
| Core (in-context) | `memory_blocks` table | Always in system prompt, bounded by `char_limit` |
| Archival (long-term) | §F: `passages` table + embeddings | Semantic search, unlimited |
| Recall (history) | `entries` table + FTS5 | Keyword/semantic search over all past turns |

## Core Memory (memory_blocks)

Agent's working memory — injected into system prompt every turn.

- **Unit**: labeled block with character limit
- **Default blocks**: `persona` (agent identity), `human` (user facts), `instructions` (standing orders)
- **Agent-editable**: via `memory_append`, `memory_replace` tools
- **Read-only option**: `read_only=1` → visible but agent tools reject edits
- **Scoped per-agent**: not per-session (persists across all sessions for that agent)
- **Rendered in prompt**: with metadata (label, description, chars_used/limit)

Key insight from Letta: memory management is a **tool-use problem**. The agent decides what to remember via tool calls, not system-level heuristics.

## Recall (entries + FTS5)

All past messages searchable via `db_query` tool (read-only SQL) or future auto-recall.

- FTS5 indexes `content` column directly (split-column schema)
- Agent can search own history: `SELECT content FROM entries_fts WHERE entries_fts MATCH ?`
- Context window manager (V7) loads recent turns; older entries accessible via search

## Future: Archival (vector recall)

From §F FUTURE:
- `passages` table (text, embedding BLOB, source_type, source_id)
- Hybrid search: FTS5 + vector similarity (RRF merge)
- Agent tool: `memory_search(query)` for explicit recall
- Auto-recall: inject top hits into system prompt at context build time

## Context Window Management

- V7: load ≤ `max_history_tokens` (60% of context window) most recent turns
- V8: never cut mid-tool-call; cut at valid turn boundary
- Cutoff notice prepended when truncated
- Future: compaction (T160-T163) — summarize old entries, reparent tree

## Letta Patterns NOT Adopted

| Letta Feature | CClaw Decision | Reason |
|---------------|---------------|--------|
| Shared blocks across agents | Not implemented | Single-user system, low priority |
| Sleeptime background agent | Not implemented | Adds complexity; agent can manage own memory |
| Stateless mode (forget between turns) | Not needed | Sessions are cheap, tree structure handles branching |
| PostgreSQL + pgvector | SQLite + future embedding column | Single-file DB, no server dependency |
| 100K char block limit | 5K default | Smaller context windows, cheaper models |
