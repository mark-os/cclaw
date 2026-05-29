# Schema & Data Model — 3-DB Architecture

Three SQLite files (all WAL mode, `busy_timeout` ≥ 5000ms):

| DB | Path | Owner | Contents |
|----|------|-------|----------|
| cclaw.db | `.cclaw/cclaw.db` | daemon (sole writer) | agents registry, agent_config, providers, kv (secrets), channel_bindings, tg_chat_sessions, spawn_queue, cron_jobs, approvals |
| agent.db | `agents/<name>/agent.db` | agent process (RW); daemon (inbox writes only) | sessions, entries, inbox, js_tools, memory_blocks, kv (agent-local) |
| journal.db | `.cclaw/journal.db` | log collector (sole writer) | log table (all stdout/stderr from daemon + agents) |

---

## cclaw.db

### agents

| Column | Type | Notes |
|--------|------|-------|
| `name` | TEXT PRIMARY KEY | |
| `status` | TEXT NOT NULL DEFAULT 'active' | active\|disabled |
| `created_at` | INTEGER DEFAULT (unixepoch()) | |
| `updated_at` | INTEGER DEFAULT (unixepoch()) | |

### agent_config

Replaces `agent.json` files. Daemon reads at fork, injects as `CCLAW_*` env vars.

| Column | Type | Notes |
|--------|------|-------|
| `agent_name` | TEXT NOT NULL | |
| `key` | TEXT NOT NULL | |
| `value` | TEXT NOT NULL | |
| PRIMARY KEY | (agent_name, key) | |

Keys: `model`, `workspace`, `tools` (JSON array), `allowed_hosts` (JSON array), `read_access` (JSON array), `max_iterations`, `shell_timeout`, `landlock_net_ports` (JSON array), `daemon_db_read` (0\|1).

### providers

| Column | Type | Notes |
|--------|------|-------|
| `name` | TEXT PRIMARY KEY | |
| `base_url` | TEXT NOT NULL | |
| `model` | TEXT NOT NULL | |
| `context_window` | INTEGER DEFAULT 128000 | |

### kv

Global config + encrypted secrets. Daemon-only access.

| Column | Type | Notes |
|--------|------|-------|
| `key` | TEXT PRIMARY KEY | dotted namespace: `provider.api_key`, `telegram_token`, etc. |
| `value` | TEXT NOT NULL | plaintext or `enc:<hex(nonce\|\|ct\|\|tag)>` for secrets |

### channel_bindings

| Column | Type | Notes |
|--------|------|-------|
| `channel_type` | TEXT NOT NULL | telegram, cli, webhook |
| `channel_id` | TEXT NOT NULL | |
| `agent_name` | TEXT NOT NULL | |
| `created_at` | INTEGER DEFAULT (unixepoch()) | |
| PRIMARY KEY | (channel_type, channel_id) | |

### tg_chat_sessions

| Column | Type | Notes |
|--------|------|-------|
| `chat_id` | INTEGER PRIMARY KEY | |
| `session_id` | INTEGER NOT NULL | |
| `agent_name` | TEXT NOT NULL | |

### spawn_queue

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `parent_agent` | TEXT NOT NULL | |
| `parent_session_id` | INTEGER NOT NULL | |
| `tool_call_id` | TEXT | for blocking: match result to parent's tool_call |
| `child_agent` | TEXT | NULL until assigned |
| `child_session_id` | INTEGER | NULL until forked |
| `task` | TEXT NOT NULL | |
| `background` | INTEGER DEFAULT 0 | |
| `depth` | INTEGER DEFAULT 1 | |
| `status` | TEXT DEFAULT 'pending' | pending\|running\|done\|failed |
| `created_at` | INTEGER DEFAULT (unixepoch()) | |

Index: `idx_spawn_pending ON spawn_queue(status) WHERE status='pending'`

### cron_jobs

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `agent_name` | TEXT NOT NULL | |
| `name` | TEXT NOT NULL | |
| `cron_expr` | TEXT NOT NULL | |
| `session_id` | INTEGER NOT NULL | |
| `task` | TEXT NOT NULL | |
| `enabled` | INTEGER DEFAULT 1 | |
| `next_run_at` | INTEGER DEFAULT 0 | |
| `last_run_at` | INTEGER | |
| `created_at` | INTEGER DEFAULT (unixepoch()) | |

### approvals

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `agent_name` | TEXT NOT NULL | |
| `session_id` | INTEGER NOT NULL | |
| `type` | TEXT NOT NULL | whitelist_host\|create_agent\|model_change\|tool_enable |
| `payload` | TEXT NOT NULL | JSON |
| `status` | TEXT DEFAULT 'pending' | pending\|approved\|denied |
| `admin_chat_id` | INTEGER | |
| `notified` | INTEGER DEFAULT 0 | |
| `created_at` | INTEGER DEFAULT (unixepoch()) | |
| `resolved_at` | INTEGER | |

Index: `idx_approvals_pending ON approvals(status) WHERE status='pending'`

---

## agents/\<name\>/agent.db

### sessions

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `name` | TEXT | |
| `leaf_id` | INTEGER DEFAULT -1 | current branch tip entry |
| `parent_session_id` | INTEGER DEFAULT -1 | sub-agent parent (-1 = top-level) |
| `depth` | INTEGER DEFAULT 0 | sub-agent depth |
| `state` | TEXT DEFAULT 'idle' | idle\|running\|waiting |
| `last_route` | TEXT | delivery target |
| `created_at` | INTEGER DEFAULT (unixepoch()) | |
| `updated_at` | INTEGER DEFAULT (unixepoch()) | |

### entries

Split-column format — no JSON parsing on LLM request hot path.

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `session_id` | INTEGER NOT NULL | |
| `parent_id` | INTEGER DEFAULT -1 | tree structure (Pi model) |
| `original_parent_id` | INTEGER | nullable, set on reparent |
| `turn_id` | INTEGER | groups entries within a turn |
| `role` | INTEGER DEFAULT 1 | 0=system, 1=user, 2=assistant, 3=tool, 4=compaction |
| `content` | TEXT | raw text |
| `tool_calls` | TEXT | JSON array, NULL if none |
| `tool_call_id` | TEXT | for role=tool only |
| `tool_name` | TEXT | for role=tool only |
| `is_error` | INTEGER DEFAULT 0 | for role=tool |
| `stop_reason` | INTEGER DEFAULT 0 | 0=none, 1=stop, 2=length, 3=tool_use, 4=error, 5=aborted |
| `model` | TEXT | which model produced this |
| `usage_in` | INTEGER | input tokens |
| `usage_out` | INTEGER | output tokens |
| `token_estimate` | INTEGER | chars/4 heuristic |
| `content_bytes` | INTEGER | byte length of content + tool_calls |
| `tool_call_count` | INTEGER DEFAULT 0 | denormalized for plan pass |
| `data` | TEXT | legacy/debug (nullable) |
| `created_at` | INTEGER DEFAULT (unixepoch()) | |

### StopReason enum

| Value | Int | Provider sources |
|-------|-----|------------------|
| stop | 1 | `"stop"`, `"end"`, `"end_turn"`, `null` |
| length | 2 | `"length"`, `"max_tokens"` |
| tool_use | 3 | `"tool_calls"`, `"function_call"`, `"tool_use"` |
| error | 4 | `"content_filter"`, `"network_error"`, unknown, missing |
| aborted | 5 | (internal — SIGTERM, user cancel, timeout) |

### tool_calls format

Provider-neutral, `args` stored as object (not stringified):

```json
[{"id":"tc_1","name":"shell_exec","args":{"cmd":"ls"}}]
```

Wire emission per-provider at stream time (V60):
- **OpenAI**: stringify `args` → `"arguments":"..."`
- **Anthropic**: `args` as `input` object
- **Google**: `args` as `functionCall.args` object

### FTS5

```sql
CREATE VIRTUAL TABLE entries_fts USING fts5(content, content=entries, content_rowid=id);
-- AFTER INSERT trigger copies content → entries_fts (role-aware)
```

### inbox

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `session_id` | INTEGER NOT NULL | |
| `source` | TEXT NOT NULL | channel origin |
| `payload` | TEXT NOT NULL | message content |
| `created_at` | INTEGER DEFAULT (unixepoch()) | |
| `consumed` | INTEGER DEFAULT 0 | |

Index: `idx_inbox_pending ON inbox(session_id, consumed) WHERE consumed = 0`

### js_tools

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `session_id` | INTEGER NOT NULL | |
| `name` | TEXT NOT NULL | UNIQUE(session_id, name) |
| `description` | TEXT | |
| `parameters_json` | TEXT | |
| `code` | TEXT NOT NULL | |

### memory_blocks

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `label` | TEXT NOT NULL UNIQUE | |
| `value` | TEXT DEFAULT '' | |
| `description` | TEXT | tells agent what block is for |
| `char_limit` | INTEGER DEFAULT 5000 | |
| `read_only` | INTEGER DEFAULT 0 | |
| `created_at` | INTEGER DEFAULT (unixepoch()) | |
| `updated_at` | INTEGER DEFAULT (unixepoch()) | |

### kv (agent-local)

Agent-local preferences only (no secrets — secrets live in cclaw.db).

| Column | Type | Notes |
|--------|------|-------|
| `key` | TEXT PRIMARY KEY | |
| `value` | TEXT NOT NULL | |

---

## journal.db

### log

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `source` | TEXT NOT NULL | "daemon", agent name, "collector" |
| `pid` | INTEGER | |
| `session_id` | INTEGER | |
| `stream` | INTEGER DEFAULT 1 | 1=stdout, 2=stderr |
| `line` | TEXT NOT NULL | |
| `created_at` | INTEGER DEFAULT (unixepoch()) | |

Indexes:
```sql
CREATE INDEX idx_log_source ON log(source, created_at);
CREATE INDEX idx_log_time ON log(created_at);
```

---

## Indexes (agent.db)

```sql
CREATE INDEX idx_entries_session ON entries(session_id, id);
CREATE INDEX idx_entries_parent ON entries(parent_id);
CREATE INDEX idx_entries_session_role ON entries(session_id, role);
CREATE INDEX idx_entries_turn ON entries(session_id, turn_id);
CREATE INDEX idx_entries_stop_reason ON entries(session_id, stop_reason) WHERE stop_reason != 0;
CREATE INDEX idx_entries_plan ON entries(parent_id, session_id, id, role, stop_reason, token_estimate, tool_call_count);
```

---

## Design decisions

1. **3-DB split** — cclaw.db for coordination (daemon sole writer), agent.db for session data (agent RW, daemon inbox-only), journal.db for logs (collector sole writer); eliminates cross-concern locking
2. **Exit code IPC** — agents signal intent via exit code; daemon reads details from agent DB post-reap; no shared-memory IPC, no pipes for structured data
3. **agent_config in cclaw.db** — replaces agent.json; daemon reads at fork, injects as env vars; single source of truth for policy
4. **Split columns over JSON** — zero cJSON parsing on LLM request hot path (V60); metadata columns enable plan pass without overflow page loads (V56)
5. **INTEGER ids** — faster joins, smaller indexes, natural ordering
6. **parent_id tree** — supports branching (Pi model); walk leaf→root via recursive CTE
7. **Inbox as table** — durable queue; survives crashes; atomic consumption via `BEGIN EXCLUSIVE`
8. **WAL mode** — multiple readers never block; writers serialize briefly on commit
9. **Log collector** — single writer to journal.db; receives fds via SCM_RIGHTS; agents don't need journal.db access
10. **CLI standalone** — opens agent.db directly; no cclaw.db needed; config from env vars or defaults
