# Schema & Data Model

## entries

Split-column format — no JSON parsing on LLM request hot path.

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY | |
| `session_id` | INTEGER NOT NULL | |
| `parent_id` | INTEGER | tree structure (Pi model) |
| `original_parent_id` | INTEGER | nullable, set on reparent |
| `turn_id` | INTEGER | groups entries within a turn |
| `role` | INTEGER NOT NULL | 0=system, 1=user, 2=assistant, 3=tool, 4=compaction |
| `content` | TEXT | raw text — user/system/assistant text; compaction summary |
| `tool_calls` | TEXT | JSON array, NULL if none; provider-neutral format |
| `tool_call_id` | TEXT | for role=tool only |
| `tool_name` | TEXT | for role=tool only — which tool produced this result |
| `is_error` | INTEGER DEFAULT 0 | for role=tool — whether result is error |
| `stop_reason` | INTEGER DEFAULT 0 | 0=none, 1=stop, 2=length, 3=tool_use, 4=error, 5=aborted |
| `model` | TEXT | for role=assistant — which model produced this |
| `usage_in` | INTEGER | input tokens |
| `usage_out` | INTEGER | output tokens |
| `token_estimate` | INTEGER | chars/4 heuristic of full wire representation |
| `content_bytes` | INTEGER | byte length of content + tool_calls combined |
| `tool_call_count` | INTEGER DEFAULT 0 | count of items in tool_calls array; denormalized for plan pass |
| `created_at` | TEXT | DEFAULT (datetime('now')) |

### StopReason enum

Normalized, provider-agnostic (Pi model):

| Value | Int | Provider `finish_reason` sources |
|-------|-----|----------------------------------|
| `stop` | 1 | `"stop"`, `"end"`, `"end_turn"`, `null` |
| `length` | 2 | `"length"`, `"max_tokens"` |
| `tool_use` | 3 | `"tool_calls"`, `"function_call"`, `"tool_use"` |
| `error` | 4 | `"content_filter"`, `"network_error"`, unknown, missing |
| `aborted` | 5 | (internal — SIGTERM, user cancel, timeout) |

### tool_calls format

Provider-neutral, `args` stored as object (not stringified):

```json
[{"id":"tc_1","name":"shell_exec","args":{"cmd":"ls"}},{"id":"tc_2","name":"file_read","args":{"path":"foo.c"}}]
```

### Wire emission

Per-provider, computed at stream time from columns (V60):

- **OpenAI**: `content` → `"content":"..."`, `tool_calls` → stringify each `args` object into `"arguments":"..."` + wrap in `{"type":"function","function":{...}}`
- **Anthropic** (future): `content` → text block, `tool_calls` → `{"type":"tool_use","id":"...","name":"...","input":{...}}`
- **Google** (future): `content` → `{text:"..."}`, `tool_calls` → `{"functionCall":{"name":"...","args":{...}}}`

### FTS5

Indexes `content` column directly (no json_extract needed).

### Migration

Legacy `data TEXT` column retained during migration (populated for old entries, NULL for new); `reshape_entry()` fallback reads `data` when split columns empty; drop `data` column after all active sessions contain only new-format entries.

---

## inbox

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY | |
| `session_id` | INTEGER NOT NULL | |
| `source` | TEXT | channel origin (telegram, cli, cron, sub-agent) |
| `payload` | TEXT | message content |
| `created_at` | TEXT | |
| `consumed` | INTEGER DEFAULT 0 | |

---

## sessions

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY | |
| `name` | TEXT | |
| `leaf_id` | INTEGER | current branch tip entry |
| `agent_name` | TEXT | |
| `parent_session_id` | INTEGER DEFAULT -1 | sub-agent parent (-1 = top-level) |
| `depth` | INTEGER DEFAULT 0 | sub-agent depth (0 = top-level) |
| `state` | TEXT DEFAULT 'idle' | idle\|running\|waiting |
| `error_count` | INTEGER DEFAULT 0 | |
| `last_route` | TEXT | delivery target: "telegram:\<chat_id\>", "cli", "cron:\<job\>", "subagent:\<sid\>" |
| `created_at` | INTEGER DEFAULT (unixepoch()) | |
| `updated_at` | INTEGER DEFAULT (unixepoch()) | |

---

## agents

Replaces filesystem-based MEMORY.md, SOUL.md, system.md, HEARTBEAT.md.

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY | |
| `name` | TEXT UNIQUE | |
| `config` | TEXT | JSON — model, tools, limits, allowed_hosts |
| `system_prompt` | TEXT | template, supports `{session_id}`, `{date}`, `{agent_name}` |
| `heartbeat` | TEXT | proactive task instructions, read on heartbeat turns |
| `created_at` | TEXT | |
| `updated_at` | TEXT | |

---

## approvals

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY | |
| `session_id` | INTEGER | |
| `agent_name` | TEXT | |
| `type` | TEXT | whitelist_host\|create_agent\|model_change\|tool_enable |
| `payload` | TEXT | JSON — request details |
| `status` | TEXT DEFAULT 'pending' | pending\|approved\|denied |
| `admin_chat_id` | INTEGER | |
| `created_at` | TEXT | |
| `resolved_at` | TEXT | |

---

## memory_blocks

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY | |
| `agent_name` | TEXT NOT NULL | |
| `label` | TEXT NOT NULL | UNIQUE(agent_name, label) |
| `value` | TEXT DEFAULT '' | |
| `description` | TEXT | tells agent what block is for |
| `char_limit` | INTEGER DEFAULT 5000 | |
| `read_only` | INTEGER DEFAULT 0 | |
| `created_at` | TEXT | |
| `updated_at` | TEXT | |

Default: zero blocks — agent creates via `memory_create`; optionally pre-seeded from `agent.json` `memory_blocks[]` on first reference.

### memory_blocks config (in `agent.json`)

```json
"memory_blocks": [
  {"label": "persona", "description": "Agent identity and tone", "value": "I am...", "char_limit": 5000, "read_only": false},
  {"label": "human", "description": "Known facts about the user", "char_limit": 5000, "read_only": false},
  {"label": "instructions", "description": "Standing orders", "value": "Always...", "read_only": true}
]
```

`read_only: true` → visible in context, agent memory tools reject edits.

---

## Agent config on disk

```
agents/<name>/
  agent.json    — model override, tool whitelist, max_iterations, workspace path, allowed_hosts
  skills/       — per-agent skill files (markdown, injected into system prompt)
```

---

## spawn_queue

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY | |
| `parent_session_id` | INTEGER NOT NULL | REFERENCES sessions(id) |
| `agent_name` | TEXT NOT NULL | |
| `task` | TEXT NOT NULL | |
| `mode` | TEXT DEFAULT 'blocking' | blocking\|background |
| `tool_call_id` | TEXT | for blocking: match result to parent's tool_call |
| `created_at` | INTEGER DEFAULT (unixepoch()) | |
| `consumed` | INTEGER DEFAULT 0 | |

---

## cron_jobs

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY | |
| `name` | TEXT NOT NULL | |
| `cron_expr` | TEXT NOT NULL | |
| `session_id` | INTEGER NOT NULL | REFERENCES sessions(id) |
| `task` | TEXT NOT NULL | |
| `enabled` | INTEGER DEFAULT 1 | |
| `next_run_at` | INTEGER DEFAULT 0 | |
| `last_run_at` | INTEGER | |
| `created_at` | INTEGER DEFAULT (unixepoch()) | |

---

## js_tools

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY | |
| `session_id` | INTEGER NOT NULL | REFERENCES sessions(id) |
| `name` | TEXT NOT NULL | UNIQUE(session_id, name) |
| `description` | TEXT | |
| `parameters_json` | TEXT | |
| `code` | TEXT NOT NULL | |

---

## tg_chat_sessions

| Column | Type | Notes |
|--------|------|-------|
| `chat_id` | INTEGER PRIMARY KEY | |
| `session_id` | INTEGER NOT NULL | REFERENCES sessions(id) |

---

## kv

| Column | Type | Notes |
|--------|------|-------|
| `key` | TEXT PRIMARY KEY | |
| `value` | TEXT NOT NULL | |

---

## Indexes

```sql
CREATE INDEX idx_entries_session ON entries(session_id, id);
CREATE INDEX idx_entries_parent ON entries(parent_id);
CREATE INDEX idx_entries_session_role ON entries(session_id, role);
CREATE INDEX idx_entries_turn ON entries(session_id, turn_id);
CREATE INDEX idx_entries_stop_reason ON entries(session_id, stop_reason) WHERE stop_reason != 0;
CREATE INDEX idx_inbox_pending ON inbox(session_id, consumed) WHERE consumed = 0;
CREATE INDEX idx_spawn_queue_pending ON spawn_queue(consumed) WHERE consumed = 0;
```

---

## FTS5

```sql
CREATE VIRTUAL TABLE entries_fts USING fts5(content, content=entries, content_rowid=id);
-- AFTER INSERT trigger copies entries.content → entries_fts
```

---

## Design decisions

1. **Split columns over JSON `data`** — zero cJSON parsing on LLM request hot path (V60); metadata columns enable plan pass without overflow page loads (V56)
2. **INTEGER ids** — faster joins, smaller indexes, natural ordering; no UUIDs (single-writer per session)
3. **parent_id tree** — supports branching (Pi model); walk leaf→root for current branch via recursive CTE
4. **Inbox as table** — durable queue; survives crashes; atomic consumption via `BEGIN EXCLUSIVE`
5. **WAL mode** — multiple readers never block; writers serialize briefly on commit
6. **`spawn_queue`** — daemon reads on signal pipe wake, forks sub-agent, marks consumed; decouples agent process from fork logic (V21)
7. **Sub-agents are sessions** — no separate tracking table; `parent_session_id` + `depth` on sessions table; V3 limits enforced via count query
8. **`tool_calls` as JSON array** — provider-neutral format; `args` stored as object (not stringified); per-provider wire formatting at emit time only

---

## Design notes

- Agent identity (prompts, memory blocks) lives in SQLite → portable by copying DB, no filesystem sync
- Agent can self-modify memory blocks via tools (`memory_append`, `memory_replace`) — just DB writes; `description` field guides agent on block purpose (visible in context, not editable by agent)
- System prompt assembled at turn start: `system_prompt` template + memory blocks (rendered w/ label, description, metadata, value) + skills
- `agents/<name>/agent.json` on disk is bootstrap/import path — loaded once to seed DB row; DB authoritative after that
- Workspace = working directory for file tools — no magic filenames, no special semantics
- `/tmp/cclaw-<session_id>/` — ephemeral per-session scratch (tool output overflow)
- Writable paths under landlock/namespace: workspace + `/tmp/cclaw-<session_id>/` + DB file
