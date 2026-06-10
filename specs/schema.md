# Schema & Data Model

Single SQLite file `cclaw.db` (WAL mode, `busy_timeout` ≥ 5000ms). Parent (CLI/daemon) is primary writer. Agent child opens same file via `CCLAW_DB` env var.

Source of truth: `templates/schema.sql` (embedded at build time as `TPL_SCHEMA_SQL`).

---

## agents

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `name` | TEXT NOT NULL UNIQUE | |
| `config` | TEXT | legacy/unused |
| `system_prompt` | TEXT | inline prompt override |
| `heartbeat` | TEXT | heartbeat prompt |
| `created_at` | INTEGER DEFAULT (unixepoch()) | |
| `updated_at` | INTEGER DEFAULT (unixepoch()) | |

## agent_config

Replaces config files. Daemon reads at fork, injects as `CCLAW_*` env vars.

| Column | Type | Notes |
|--------|------|-------|
| `agent_name` | TEXT NOT NULL | |
| `key` | TEXT NOT NULL | |
| `value` | TEXT NOT NULL | |
| PRIMARY KEY | (agent_name, key) | |

**Absent-key semantics** (V124): missing key = conservative system default, NOT unlimited. Daemon always injects `CCLAW_TOOLS` and `CCLAW_ALLOWED_HOSTS` env vars at fork — even when no agent_config rows exist.

| Key | Absent default | Notes |
|-----|----------------|-------|
| `model` | global provider model | from cclaw.db kv `provider.model` |
| `workspace` | `agents/<name>/workspace` | auto-derived from agent name |
| `tools` | `["file_read","file_write","js_eval","memory_create","memory_append","memory_replace","request_config"]` | V119 minimum set |
| `allowed_hosts` | `[]` (empty — no network) | must be explicitly granted |
| `read_access` | `[]` (none) | |
| `max_iterations` | 25 | |
| `shell_timeout` | 30 | seconds |
| `memory_limit` | 268435456 (256MB) | bytes; 0 = unlimited |
| `cpu_limit` | 300 | seconds; 0 = unlimited |
| `daemon_db_read` | 0 | |
| `sandbox` | `sandbox` | `"sandbox"` = namespace isolation (V22a); `"none"` = no isolation |

**Sub-agent inheritance** (V123): child config = intersection(child's own agent_config, parent's agent_config). Child ⊥ exceed parent.

## Full config env var reference

All injected at fork by daemon/CLI. Agent reads via `config_load_from_env()`.

| Env var | Source | Default | Notes |
|---------|--------|---------|-------|
| `CCLAW_AGENT_NAME` | daemon derives | "default" | agent identity |
| `CCLAW_DB` | daemon derives | `.cclaw/cclaw.db` | path to DB |
| `CCLAW_WORKSPACE` | agent_config `workspace` | `agents/<name>/workspace` | rw directory |
| `CCLAW_MODEL` | agent_config `model` | global provider model | LLM model name |
| `CCLAW_TOOLS` | agent_config `tools` | V119 default set | comma-separated whitelist |
| `CCLAW_ALLOWED_HOSTS` | agent_config `allowed_hosts` | (empty) | comma-separated hostnames |
| `CCLAW_MAX_ITERATIONS` | agent_config `max_iterations` | 25 | tool loop cap |
| `CCLAW_SHELL_TIMEOUT` | agent_config `shell_timeout` | 30 | seconds |
| `CCLAW_MEMORY_LIMIT` | agent_config `memory_limit` | 268435456 | bytes; 0=unlimited |
| `CCLAW_CPU_LIMIT` | agent_config `cpu_limit` | 300 | seconds; 0=unlimited |
| `CCLAW_PROVIDER_BASE_URL` | cclaw.db kv / provider | `https://openrouter.ai/api/v1` | |
| `CCLAW_MAX_TOKENS` | cclaw.db kv | 4096 | max output tokens |
| `CCLAW_CONTEXT_WINDOW` | cclaw.db kv | 65536 | model context size |
| `CCLAW_CONTEXT_THRESHOLD` | cclaw.db kv | 0.6 | triggers compaction/truncation |
| `CCLAW_COMPACTION_TARGET` | cclaw.db kv | 0.3 | post-compaction target |
| `CCLAW_COMPACTION` | cclaw.db kv | 1 | bool: enable compaction |
| `CCLAW_AUTO_RECALL` | cclaw.db kv | 1 | bool: FTS5 auto-recall |
| `CCLAW_RECALL_MAX_TOKENS` | cclaw.db kv | 500 | max recalled context |
| `CCLAW_STREAM` | CLI sets | 0 | bool: SSE streaming |
| `CCLAW_MODE` | parent sets | (none) | `cli` or `daemon` |
| `CCLAW_LOG_LEVEL` | cclaw.db kv | info | error\|info\|debug\|trace |
| `CCLAW_TOKEN_RATE_LIMIT` | cclaw.db kv | 1000000 | hourly token cap |
| `CCLAW_SAVE_REASONING` | cclaw.db kv | 0 | bool: persist reasoning |
| `CCLAW_SAVE_USAGE` | cclaw.db kv | 0 | bool: persist usage stats |
| `CCLAW_PATH` | CLI sets | (none) | CWD for ro file_read |
| `CCLAW_YOLO` | CLI `-y` flag | (none) | disables sandbox |
| `CCLAW_SANDBOX` | agent_config `sandbox` | `sandbox` | `sandbox` \| `none` (V22a) |
| `CCLAW_SECRET_<NAME>` | cclaw.db kv (encrypted) | — | decrypted at fork, cleared after read |

## providers

| Column | Type | Notes |
|--------|------|-------|
| `name` | TEXT PRIMARY KEY | |
| `base_url` | TEXT NOT NULL | |
| `endpoint_type` | TEXT NOT NULL DEFAULT 'openai' | openai\|gemini\|anthropic |
| `api_key_env` | TEXT NOT NULL DEFAULT '' | env var name for API key |
| `default_model_id` | TEXT | |
| `context_window` | INTEGER DEFAULT 128000 | |
| `token_rate_limit` | INTEGER DEFAULT 0 | |

## kv

Global config + encrypted secrets. Daemon-only write access.

| Column | Type | Notes |
|--------|------|-------|
| `key` | TEXT PRIMARY KEY | dotted namespace: `provider.api_key`, `telegram_token`, etc. |
| `value` | TEXT NOT NULL | plaintext or `enc:<hex(nonce\|\|ct\|\|tag)>` for secrets |

## channel_bindings

| Column | Type | Notes |
|--------|------|-------|
| `channel_type` | TEXT NOT NULL | telegram, cli, webhook |
| `channel_id` | TEXT NOT NULL | |
| `agent_name` | TEXT NOT NULL | |
| `created_at` | INTEGER DEFAULT (unixepoch()) | |
| `updated_at` | INTEGER DEFAULT (unixepoch()) | |
| PRIMARY KEY | (channel_type, channel_id) | |

## tg_chat_sessions

| Column | Type | Notes |
|--------|------|-------|
| `chat_id` | INTEGER PRIMARY KEY | |
| `session_id` | INTEGER NOT NULL | |
| `agent_name` | TEXT NOT NULL DEFAULT '' | |

## cron_jobs

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `agent_name` | TEXT NOT NULL DEFAULT '' | |
| `name` | TEXT NOT NULL | |
| `cron_expr` | TEXT NOT NULL | |
| `session_id` | INTEGER NOT NULL | |
| `task` | TEXT NOT NULL | |
| `enabled` | INTEGER DEFAULT 1 | |
| `next_run_at` | INTEGER DEFAULT 0 | |
| `last_run_at` | INTEGER | |
| `created_at` | INTEGER DEFAULT (unixepoch()) | |

## channels

| Column | Type | Notes |
|--------|------|-------|
| `name` | TEXT PRIMARY KEY | |
| `type` | TEXT NOT NULL | telegram, webhook, custom |
| `binary_path` | TEXT NOT NULL | path to channel process binary |
| `status` | TEXT NOT NULL DEFAULT 'active' | active\|failed |
| `pid` | INTEGER | daemon tracks running process |
| `created_at` | INTEGER DEFAULT (unixepoch()) | |

## channel_events

Inbound events from channel processes → daemon. Consumed in FIFO order.

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `channel_name` | TEXT NOT NULL | |
| `event_type` | TEXT NOT NULL | message, callback, etc. |
| `payload` | TEXT NOT NULL | JSON |
| `created_at` | INTEGER DEFAULT (unixepoch()) | |

## channel_outbox

Daemon → channel process delivery queue.

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `channel_name` | TEXT NOT NULL | |
| `session_id` | INTEGER NOT NULL | |
| `payload` | TEXT NOT NULL | JSON |
| `status` | TEXT NOT NULL DEFAULT 'pending' | pending\|delivered\|failed |
| `created_at` | INTEGER DEFAULT (unixepoch()) | |
| `acked_at` | INTEGER | |

Index: `idx_channel_outbox_pending ON channel_outbox(channel_name, status) WHERE status='pending'`

## channel_state

Channel-private persistent kv (offsets, cursors, tokens).

| Column | Type | Notes |
|--------|------|-------|
| `channel_name` | TEXT NOT NULL | |
| `key` | TEXT NOT NULL | |
| `value` | TEXT NOT NULL | |
| PRIMARY KEY | (channel_name, key) | |

---

## sessions

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `name` | TEXT | |
| `leaf_id` | INTEGER DEFAULT -1 | current branch tip entry |
| `agent_name` | TEXT | scopes session to agent |
| `parent_session_id` | INTEGER DEFAULT -1 | sub-agent parent (-1 = top-level) |
| `depth` | INTEGER DEFAULT 0 | sub-agent depth |
| `state` | TEXT DEFAULT 'idle' | idle\|running\|waiting |
| `last_route` | TEXT | delivery target |
| `cache_break_after` | INTEGER DEFAULT -1 | entry ID after which cache breaks |
| `last_interaction_id` | TEXT | |
| `last_synced_entry_id` | INTEGER | |
| `created_at` | INTEGER DEFAULT (unixepoch()) | |
| `updated_at` | INTEGER DEFAULT (unixepoch()) | |

## entries

Split-column format — no JSON parsing on LLM request hot path (V60).

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `session_id` | INTEGER NOT NULL | |
| `parent_id` | INTEGER DEFAULT -1 | tree structure (Pi model) |
| `original_parent_id` | INTEGER | nullable, set on reparent |
| `turn_id` | INTEGER | groups entries within a turn |
| `type` | TEXT DEFAULT 'user_message' | entry type discriminator |
| `part_index` | INTEGER DEFAULT 0 | ordering within multi-part entries |
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
| `cost_nano` | INTEGER | cost in nanodollars |
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
-- AFTER INSERT trigger copies content → entries_fts (role-aware: tool entries prefix with tool_name)
```

## tool_calls (table)

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `session_id` | INTEGER NOT NULL | |
| `entry_id` | INTEGER NOT NULL | |
| `call_id` | TEXT NOT NULL | matches tool_call JSON id |
| `name` | TEXT NOT NULL | tool name |
| `arguments` | TEXT | JSON |
| `result_entry_id` | INTEGER | links to tool_result entry |
| `status` | TEXT DEFAULT 'pending' | pending\|completed\|failed |
| `resolved_by` | TEXT | |
| `resolved_at` | INTEGER | |

Indexes: `idx_tool_calls_entry(entry_id)`, `idx_tool_calls_session(session_id, status)`

## inbox

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `session_id` | INTEGER NOT NULL | |
| `source` | TEXT NOT NULL | channel origin |
| `payload` | TEXT NOT NULL | message content |
| `created_at` | INTEGER DEFAULT (unixepoch()) | |
| `consumed` | INTEGER DEFAULT 0 | |

Index: `idx_inbox_pending ON inbox(session_id, consumed) WHERE consumed = 0`

## js_tools

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `session_id` | INTEGER NOT NULL | |
| `name` | TEXT NOT NULL | UNIQUE(session_id, name) |
| `description` | TEXT | |
| `parameters_json` | TEXT | |
| `code` | TEXT NOT NULL | |

## memory_blocks

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `agent_name` | TEXT NOT NULL | scopes block to agent |
| `label` | TEXT NOT NULL | |
| `value` | TEXT DEFAULT '' | |
| `description` | TEXT | tells agent what block is for |
| `char_limit` | INTEGER DEFAULT 5000 | |
| `read_only` | INTEGER DEFAULT 0 | |
| `created_at` | INTEGER DEFAULT (unixepoch()) | |
| `updated_at` | INTEGER DEFAULT (unixepoch()) | |
| UNIQUE | (agent_name, label) | |

---

## Indexes

```sql
CREATE INDEX idx_entries_session ON entries(session_id, id);
CREATE INDEX idx_entries_parent ON entries(parent_id);
CREATE INDEX idx_entries_session_role ON entries(session_id, role);
CREATE INDEX idx_entries_turn ON entries(session_id, turn_id);
CREATE INDEX idx_entries_turn_type ON entries(session_id, turn_id, type, part_index);
CREATE INDEX idx_entries_stop_reason ON entries(session_id, stop_reason) WHERE stop_reason != 0;
CREATE INDEX idx_entries_plan ON entries(parent_id, session_id, id, role, stop_reason, token_estimate, tool_call_count);
CREATE INDEX idx_tool_calls_entry ON tool_calls(entry_id);
CREATE INDEX idx_tool_calls_session ON tool_calls(session_id, status);
CREATE INDEX idx_inbox_pending ON inbox(session_id, consumed) WHERE consumed = 0;
CREATE INDEX idx_channel_outbox_pending ON channel_outbox(channel_name, status) WHERE status='pending';
```

---

## Design decisions

1. **Single DB** — all state in one `cclaw.db`; WAL mode allows concurrent readers; parent serializes writes; simplifies deployment (one file to back up/move)
2. **Exit code IPC** — agents signal intent via exit code; daemon reads details from DB post-reap; no shared-memory IPC, no pipes for structured data
3. **agent_config in DB** — daemon reads at fork, injects as env vars; single source of truth for policy
4. **Split columns over JSON** — SQLite `json_object()` builds wire JSON directly from columns (V60); metadata columns enable plan pass without overflow page loads (V56)
5. **INTEGER ids** — faster joins, smaller indexes, natural ordering
6. **parent_id tree** — supports branching (Pi model); walk leaf→root via recursive CTE
7. **Inbox as table** — durable queue; survives crashes; atomic consumption via `BEGIN EXCLUSIVE`
8. **WAL mode** — multiple readers never block; writers serialize briefly on commit
9. **Agent scoping** — `agent_name` column on sessions + memory_blocks scopes data per-agent within the shared DB; no filesystem isolation needed for data separation
10. **CLI standalone** — opens cclaw.db directly; config from env vars or defaults; no daemon needed
