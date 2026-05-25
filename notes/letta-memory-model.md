# Letta Persistent Agent Memory Model

Notes from reviewing `reference/letta` source code. Focus: how a single agent persists and manages memory across turns.

## Three-Tier Memory Architecture

### 1. Core Memory (in-context blocks)

The agent's working memory — injected directly into the LLM context window every turn.

- **Unit**: `Block` — a labeled chunk of text with a character limit
- **Default limit**: 100,000 chars per block (`CORE_MEMORY_BLOCK_CHAR_LIMIT`)
- **Standard blocks**: `persona` (who the agent is) and `human` (what it knows about the user)
- **Arbitrary blocks**: agents can have any number of labeled blocks
- **Rendered as XML** in the system prompt: `<memory_blocks><persona>...</persona><human>...</human></memory_blocks>`
- **Metadata shown to LLM**: description, read_only flag, chars_current, chars_limit
- **Persistence**: stored in a `blocks` table, linked to agents via `blocks_agents` junction table
- **Shared blocks**: a single block can be attached to multiple agents (shared state)

Agent tools for editing core memory:
- `core_memory_append(label, content)` — append text to a block
- `core_memory_replace(label, old_content, new_content)` — find-and-replace
- `memory_replace(label, old_string, new_string)` — v2, enforces uniqueness of old_string
- `memory_insert(label, new_string, insert_line)` — insert at line number
- `memory_rethink(label, new_memory)` — full rewrite of a block
- `memory_apply_patch(label, patch)` — unified-diff style patching, supports multi-block ops
- `memory(command, path, ...)` — git-style unified tool (create/str_replace/insert/delete/rename)

### 2. Archival Memory (long-term, embedding-based)

Unlimited persistent storage searched by semantic similarity.

- **Unit**: `Passage` — text + embedding vector
- **Storage**: `passages` table with pgvector column (padded to MAX_EMBEDDING_DIM=4096)
- **Organization**: passages belong to an `Archive`, which can be shared between agents
- **Tags**: passages can have tags for filtered search
- **Search**: semantic similarity via embeddings, optionally filtered by tags and date range

Agent tools:
- `archival_memory_insert(content, tags)` — store a passage with optional tags
- `archival_memory_search(query, tags, tag_match_mode, top_k, start/end_datetime)` — semantic search

### 3. Recall Memory (conversation history search)

All past messages, searchable via hybrid text+semantic search.

- **Unit**: `Message` — standard chat message (role, content, tool_calls, etc.)
- **Storage**: `messages` table
- **Search**: hybrid (FTS + vector similarity), filterable by role, date range
- **In-context window**: agent tracks `message_ids` (JSON column) — the subset currently visible to the LLM

Agent tool:
- `conversation_search(query, roles, limit, start_date, end_date)` — hybrid search over all past messages

## Context Window Management

### In-Context Messages

The agent maintains a sliding window of messages in `message_ids`. This is the conversation the LLM actually sees. When the context fills up, compaction kicks in.

### Compaction (Summarization)

Configured per-agent via `compaction_settings` (stored as JSON column on agent):

```
CompactionSettings:
  model: str | None          # summarizer model (defaults to cheap model like haiku)
  mode: "sliding_window" | "all" | "self_compact_sliding_window" | "self_compact_all"
  sliding_window_percentage: float  # target % of context to keep (default ~0.30)
  clip_chars: int | None     # max summary length (default 50000)
  prompt: str | None         # custom summarization prompt
```

**Sliding window** (default): evicts oldest messages, summarizes them, keeps a summary message at the front of the conversation. Iteratively grows the eviction window until post-summarization tokens fit within the target percentage.

**All**: summarizes the entire conversation into a single summary, replaces all messages.

**Self-compact variants**: the agent itself generates the summary (uses its own tools/context) rather than a separate summarizer model.

The summary is packaged as a system_alert message: "Note: prior messages have been hidden from view due to conversation memory constraints. The following is a summary..."

### Sleeptime (Background Memory Agent)

When `enable_sleeptime=True`, a background `VoiceSleeptimeAgent` processes conversations offline:

1. Receives message transcripts from the main agent
2. Calls `store_memories(chunks)` — extracts important passages and inserts them into the main agent's archival memory
3. Calls `rethink_user_memory(new_memory)` — rewrites the main agent's core memory blocks with updated information
4. Has its own tool rules: `store_memories → rethink_user_memory → finish_rethinking_memory`

This separates the "thinking about what to remember" work from the real-time conversation.

## Persistence Model (PostgreSQL)

```
agents
  ├── id, name, system (prompt), agent_type
  ├── message_ids (JSON array — in-context window)
  ├── llm_config, embedding_config, compaction_settings
  ├── enable_sleeptime, message_buffer_autoclear
  └── relationships:
        ├── blocks_agents → blocks (core memory)
        ├── tools_agents → tools
        ├── archives_agents → archives → passages (archival memory)
        └── messages (all conversation history)

blocks
  ├── id, label, value, limit, description
  ├── read_only, is_template, metadata
  └── can be shared across agents

passages
  ├── id, text, embedding (vector), tags
  ├── archive_id, file_id, source_id
  └── embedding_config

messages
  ├── id, role, content, tool_calls
  ├── agent_id, created_at
  └── searchable via FTS + vector similarity
```

### Stateless Mode

`message_buffer_autoclear=True`: agent forgets all messages between turns. Still retains core memory blocks and archival/recall access. Useful for workflow agents that don't need conversation continuity.

## Key Design Decisions (relevant to CClaw)

1. **Blocks as the unit of core memory** — not a single blob. Each block has a label, limit, and can be independently edited. This gives the agent structured, bounded working memory.

2. **Agent edits its own memory** — the LLM has tools to modify its own core memory blocks. This is the key insight: memory management is a tool-use problem, not a system-level one.

3. **Three tiers map to different access patterns**:
   - Core: always in context (fast, limited)
   - Archival: semantic search (unlimited, slower)
   - Recall: conversation search (all history, hybrid search)

4. **Compaction is configurable per-agent** — different agents can have different summarization strategies. The summary becomes a system message at the front of the conversation.

5. **Background memory processing** — sleeptime agents offload memory management to avoid slowing down real-time responses.

6. **Shared blocks** — multiple agents can share the same memory block (e.g., shared knowledge base or user profile).

7. **Character limits, not token limits** — blocks use char limits (100k default) which is simpler to enforce and reason about than token counts.
