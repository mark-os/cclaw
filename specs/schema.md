# Schema & Data Model

Single SQLite file `cclaw.db` (WAL mode, `busy_timeout` 5000ms). CLI and daemon are peers sharing one source of truth; per-session ownership (`sessions.owner_instance` → `processes`) makes recovery owner-scoped so a live peer's in-flight sessions are never stomped.

Source of truth: `templates/schema.sql` (embedded at build time as `TPL_SCHEMA_SQL`).

---

## config

Registry-backed global settings (`src/config_registry.c`). Every key is declared in a static C table with a default and a description; `config_registry_sync()` mirrors those into `default_value`/`description` at startup (code-owned columns, always overwritten). `value` is the operator/agent override — `NULL` means "use default", and the effective value is always `COALESCE(value, default_value)`. `config_set()` rejects unregistered keys: no anonymous writes, so every row in this table is self-describing. Holds **no secrets** — encrypted values live in `secrets` (scope `system` for provider keys).

Extensions register keys too: `extension_install` ingests the manifest's `config[]` as rows keyed `<ext>.<key>` (dots are reserved as the namespace separator, so core keys — which never contain a dot — can't collide). Same ownership contract: install refreshes `default_value`/`description` and drops undeclared keys; `value` is untouched. `config_set()` accepts a key if it's in the C registry *or* has an extension-registered row.

`SELECT key, value, default_value, description FROM config` gives an agent the complete knob inventory: what exists, what it does, what it's set to, and whether it's an override.

| Column | Type | Notes |
|--------|------|-------|
| `key` | TEXT PRIMARY KEY | C-registry key, or extension-registered `<ext>.<key>` |
| `value` | TEXT | override; NULL = use default |
| `default_value` | TEXT | code-owned, resynced every startup |
| `description` | TEXT | code-owned, resynced every startup |

---

## providers

LLM API endpoints. Multiple providers enable fallback routing.

| Column | Type | Notes |
|--------|------|-------|
| `name` | TEXT PRIMARY KEY | |
| `base_url` | TEXT NOT NULL | |
| `endpoint_type` | TEXT NOT NULL DEFAULT 'openai' | openai / gemini / anthropic |
| `api_key_env` | TEXT NOT NULL DEFAULT '' | env var name holding the key |
| `default_model` | TEXT | |
| `context_window` | INTEGER DEFAULT 128000 | |
| `priority` | INTEGER NOT NULL DEFAULT 0 | lower = preferred |
| `status` | TEXT NOT NULL DEFAULT 'healthy' | healthy / degraded / down |

---

## models

Per-model routing metadata + lifetime stats. The router picks by `(priority, status)`.

| Column | Type | Notes |
|--------|------|-------|
| `id` | TEXT PRIMARY KEY | routing key (e.g. `openrouter/deepseek-v4-flash`) |
| `provider_name` | TEXT NOT NULL | FK-ish to `providers.name` |
| `model` | TEXT NOT NULL | wire model id sent to the provider |
| `sub_provider` | TEXT | provider-specific routing hint (OpenRouter upstream) |
| `context_window` | INTEGER DEFAULT 128000 | |
| `max_output_tokens` | INTEGER | |
| `capabilities` | TEXT DEFAULT '[]' | JSON array of capability tags |
| `priority` | INTEGER NOT NULL DEFAULT 0 | lower = preferred |
| `status` | TEXT NOT NULL DEFAULT 'healthy' | healthy / degraded / down |
| `degraded_until` | INTEGER | unixepoch; auto-heal after this time |
| `total_requests` | INTEGER DEFAULT 0 | lifetime counter |
| `total_tokens_in` | INTEGER DEFAULT 0 | |
| `total_tokens_out` | INTEGER DEFAULT 0 | |
| `total_cost_nano` | INTEGER DEFAULT 0 | nanodollars |
| `error_count_5xx` | INTEGER DEFAULT 0 | |
| `error_count_429` | INTEGER DEFAULT 0 | |
| `last_success_at` | INTEGER | |
| `last_error_at` | INTEGER | |
| `synced_at` | INTEGER | last remote metadata sync |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

Index: `idx_models_routing ON models(priority, status)`.

---

## llm_jobs

Transient work queue. The daemon poll loop writes one row per LLM request; a worker thread claims it, runs the request, deletes on completion.

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `session_id` | INTEGER NOT NULL | |
| `agent_name` | TEXT NOT NULL DEFAULT 'default' | |
| `recall` | INTEGER NOT NULL DEFAULT 0 | whether to run FTS recall before this request |
| `job_type` | INTEGER NOT NULL DEFAULT 0 | 0 = normal turn, others reserved |
| `status` | TEXT NOT NULL DEFAULT 'pending' | pending / claimed / done |
| `claimed_at` | INTEGER | timestamp when worker started |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

Index: `idx_llm_jobs_pending ON llm_jobs(status) WHERE status='pending'`.

---

## llm_responses

Raw LLM response archive for forensics and debugging. One row per HTTP response (success or failure).

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `session_id` | INTEGER NOT NULL | |
| `turn_id` | INTEGER NOT NULL | joins back to entries |
| `model` | TEXT | |
| `status` | TEXT NOT NULL | ok / empty / malformed / http_<code> / timeout / network_error |
| `provider_id` | TEXT | provider's own response id (`$.id`), NULL if absent |
| `body` | BLOB | JSONB when parseable, raw text otherwise |
| `request_body` | BLOB | JSONB of sent payload (failures only); NULL on success |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

Retention controlled by config key `llm_response_archive_max`: >0 keeps most recent N, 0 disables, <0 keeps all.

Index: `idx_llm_responses_turn ON llm_responses(session_id, turn_id)`.

---

## extensions

Shared extension registry. Each extension is a directory in `~/.cclaw/extensions/<name>` containing JS files for tools, hooks, and channels.

| Column | Type | Notes |
|--------|------|-------|
| `name` | TEXT PRIMARY KEY | |
| `path` | TEXT NOT NULL | directory path |
| `version` | TEXT DEFAULT '0.0.0' | |
| `owner_agent` | TEXT | who promoted it (NULL for builtin) |
| `published` | INTEGER NOT NULL DEFAULT 0 | single publish flag |
| `builtin` | INTEGER NOT NULL DEFAULT 0 | |
| `enabled` | INTEGER NOT NULL DEFAULT 1 | |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

---

## agents

Agent identity and per-agent config. Name is the primary key — no integer id.

| Column | Type | Notes |
|--------|------|-------|
| `name` | TEXT PRIMARY KEY | |
| `model` | TEXT | override; NULL uses global default |
| `provider` | TEXT | override; NULL uses global default |
| `system_prompt` | TEXT | inline prompt override |
| `description` | TEXT | |
| `max_iterations` | INTEGER DEFAULT 25 | tool loop cap per turn |
| `max_output_tokens` | INTEGER | per-request cap |
| `shell_timeout` | INTEGER DEFAULT 30 | seconds |
| `sandbox_profile` | TEXT DEFAULT 'standard' | host / trusted / standard / restricted |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

---

## grants

Normalized capability grants for agents. Replaces the old `agent_config` key-value approach with typed rows.

| Column | Type | Notes |
|--------|------|-------|
| `agent_name` | TEXT NOT NULL | |
| `kind` | TEXT NOT NULL | capability axis: `tool`, `host`, `read_path`, `write_path` |
| `value` | TEXT NOT NULL | the granted name/pattern |
| `approval_mode` | TEXT NOT NULL DEFAULT 'silent' | only for kind='tool' (see below) |
| `expires_at` | INTEGER | unixepoch; NULL = permanent. Row is dead after this time. |
| `created_at` | INTEGER DEFAULT (unixepoch()) | |
| PRIMARY KEY | (agent_name, kind, value) | |

`approval_mode` values (Axis B of authority, kind='tool' only):
- `silent` — tool runs freely (default).
- `always` — every call parks the session for human approval.
- `tool_decides` — a predicate decides; absent a predicate, fails closed to `always`.

---

## approvals

Audit log for tool-call approvals. When a tool call is parked (`sessions.state` = `awaiting_approval`), a row is inserted here; resolution unparks the session.

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `session_id` | INTEGER NOT NULL | |
| `tool_call_id` | TEXT | |
| `tool_name` | TEXT | |
| `action` | TEXT | |
| `args_json` | TEXT | |
| `resolve` | TEXT NOT NULL DEFAULT 'rerun' | `rerun` = re-issue the frozen tool call on approval; `apply` = apply a capability grant (request_config) |
| `state` | TEXT NOT NULL DEFAULT 'pending' | pending / approved / denied |
| `decided_via` | TEXT | who/what resolved it |
| `requested_at` | INTEGER DEFAULT (unixepoch()) | |
| `expires_at` | INTEGER | park deadline; fail-closed deny if reached |

Index: `approvals_pending ON approvals(session_id, state) WHERE state='pending'`.

`approvals.action` values beyond tool names: `sensitive` (sensitivity-axis park — ALWAYS is coerced to ONCE at resolve) and `secret_bind` (credential-binding park — ALWAYS on a url-carrying call records the binding).

---

## sensitive_targets

Sensitivity axis (specs/trust.md): operator-owned labels on targets, global — not per-agent. The one place a label subtracts from authority: labeled hosts ride every network-tier call as proxy deny-before-allow rules, and dispatch parks any call whose args reference one. Written only by `cclaw sensitive add|rm` (no agent tool).

| Column | Type | Notes |
|--------|------|-------|
| `kind` | TEXT NOT NULL DEFAULT 'host' | only `host` in v1 |
| `value` | TEXT NOT NULL | exact host or `.suffix` rule; bare domains cover subdomains |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |
| PRIMARY KEY | (kind, value) | |

---

## secret_hosts

Fail-closed credential rule (specs/trust.md): a secret may only be submitted to hosts it is bound to. Zero bindings → first use parks; shell/js calls carrying a secret get their egress narrowed to the union of bound hosts. Rows come from `cclaw secret-bind` or from ALWAYS approvals of url-carrying parks ("approve & bind").

| Column | Type | Notes |
|--------|------|-------|
| `secret_name` | TEXT NOT NULL | matches the `{{SECRET:name}}` placeholder |
| `host` | TEXT NOT NULL | exact host or `.suffix` rule |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |
| PRIMARY KEY | (secret_name, host) | |

---

## secrets

Secret store (specs/security.md): the single home for every encrypted value, all AEAD-sealed with the master key. `scope` splits two audiences: `agent` secrets feed the per-call snapshot (`secrets_snapshot()` merges them with the env-collected `CCLAW_SECRET_*` base — env wins on a name collision) and are usable via `{{SECRET:name}}`; `system` secrets are provider API keys consumed by the daemon (`db_secret_get_system()`) and are **excluded from the snapshot**, so an agent can never interpolate them. `status='pending'` is provenance/UX only; enforcement is `secret_hosts` having zero rows for the name (fail-closed, same as any other secret).

| Column | Type | Notes |
|--------|------|-------|
| `name` | TEXT PRIMARY KEY | `^[A-Z][A-Z0-9_]*$` — becomes `CCLAW_SECRET_<name>` / `{{SECRET:name}}` |
| `value` | TEXT NOT NULL | `enc:<hex(...)>`, never plaintext |
| `status` | TEXT NOT NULL DEFAULT 'active' | `active` \| `pending` |
| `source` | TEXT NOT NULL DEFAULT 'operator' | `operator` (`cclaw secret set`) \| `generated` (`secret_create` tool) \| `quarantine` (DLP capture) |
| `scope` | TEXT NOT NULL DEFAULT 'agent' | `agent` (interpolatable/injectable) \| `system` (provider keys, daemon-only) |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

Written by: `cclaw secret set\|rm\|list` (operator verb), the `secret_create` tool, `configure_provider`/`admin_set_key` (scope `system`), and `tool_result_postprocess_q`/`inbox_insert_scanned` (DLP quarantine, auto-named `PENDING_<RULEID>_<n>`). `resolve_approval` flips `pending` → `active` when a `secret_bind` ALWAYS-approval records the name's first `secret_hosts` binding.

---

## agent_extensions

Links agents to their enabled extensions.

| Column | Type | Notes |
|--------|------|-------|
| `agent_name` | TEXT | NULL = global |
| `extension_name` | TEXT NOT NULL | FK-ish to `extensions.name` |
| `config` | TEXT | JSON config blob |
| `enabled` | INTEGER NOT NULL DEFAULT 1 | |
| PRIMARY KEY | (agent_name, extension_name) | |

---

## channels

Channel processes managed by the daemon. The extension system writes these rows.

| Column | Type | Notes |
|--------|------|-------|
| `name` | TEXT PRIMARY KEY | |
| `extension_name` | TEXT NOT NULL DEFAULT '' | |
| `type` | TEXT NOT NULL DEFAULT '' | telegram / webhook / custom |
| `binary_path` | TEXT NOT NULL DEFAULT '' | path to channel process |
| `status` | TEXT NOT NULL DEFAULT 'draft' | see lifecycle below |
| `pid` | INTEGER | daemon tracks the running process |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

Status lifecycle: `draft` → `validated` → `active` → `broken`.
- `extension_install()` inserts as `draft`.
- `cclaw --channel <name> --check` validates config → `validated`.
- `--activate` promotes → `active`; `channel_launch_all()` only execs channels in this state.
- `channel_reap()` crash-loop flap detection writes `broken`; another `--check` retries the cycle.

---

## channel_state

Channel-private persistent key-value store (offsets, cursors, tokens).

| Column | Type | Notes |
|--------|------|-------|
| `channel_name` | TEXT NOT NULL | |
| `key` | TEXT NOT NULL | |
| `value` | TEXT | |
| PRIMARY KEY | (channel_name, key) | |

---

## channel_routes

Routes inbound channel messages to agents/sessions. Replaces the old `channel_bindings` and `tg_chat_sessions` tables.

| Column | Type | Notes |
|--------|------|-------|
| `channel_name` | TEXT NOT NULL | |
| `channel_id` | TEXT NOT NULL DEFAULT '*' | `*` = default route for the channel |
| `agent_name` | TEXT | target agent |
| `session_id` | INTEGER | pin to a specific session (optional) |
| PRIMARY KEY | (channel_name, channel_id) | |

---

## sessions

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `name` | TEXT | human label |
| `agent_name` | TEXT | scopes session to agent |
| `channel_name` | TEXT | originating channel |
| `channel_id` | TEXT | originating channel id |
| `parent_session_id` | INTEGER DEFAULT -1 | sub-agent parent (-1 = top-level) |
| `parent_tool_call_id` | TEXT | the tool_call that spawned this sub-session |
| `depth` | INTEGER NOT NULL DEFAULT 0 | sub-agent nesting depth |
| `tool_filter` | TEXT | JSON array of tool names (see below) |
| `state` | TEXT NOT NULL DEFAULT 'idle' | see state machine below |
| `owner_instance` | TEXT | FK to `processes.instance_id`; NULL ⟺ idle |
| `turn_iteration` | INTEGER NOT NULL DEFAULT 0 | iteration within current turn |
| `leaf_id` | INTEGER DEFAULT -1 | current branch tip entry |
| `last_route` | TEXT | delivery target |
| `last_interaction_id` | TEXT | |
| `last_synced_entry_id` | INTEGER | |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |
| `updated_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

Index: `idx_sessions_owner ON sessions(owner_instance) WHERE owner_instance IS NOT NULL`.

### sessions.state values

| State | Meaning |
|-------|---------|
| `idle` | No work in progress; `owner_instance` is NULL |
| `llm_running` | LLM request in flight on a worker thread |
| `tool_running` | Tool execution in progress |
| `compacting` | Context compaction running |
| `rate_limited` | Parked waiting for rate-limit window |
| `awaiting_agent` | Parked for a sub-agent to complete |
| `awaiting_approval` | Parked for human approval of a tool call |

Transitions are guarded by a CAS in `session_set_state()` (`src/db.c`): idle clears `owner_instance`, any busy state stamps it. `turn_iteration` resets to 0 on transition to idle.

### sessions.tool_filter

A JSON array of tool names frozen at sub-agent spawn time (`src/main.c`). Acts as a **positive scope** intersected with grants per turn — it can never widen authority, only shrink it. NULL means unrestricted (uses grants alone).

---

## entries

Split-column format — no JSON parsing on LLM request hot path. `llm_payload.c` builds the wire JSON directly from columns via `json_object()`.

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `session_id` | INTEGER NOT NULL | |
| `parent_id` | INTEGER NOT NULL DEFAULT -1 | tree structure (Pi model); -1 = root |
| `original_parent_id` | INTEGER | set on reparent (compaction) |
| `turn_id` | INTEGER | groups entries within a turn |
| `type` | TEXT NOT NULL DEFAULT 'user_message' | entry type discriminator |
| `part_index` | INTEGER NOT NULL DEFAULT 0 | ordering within multi-part entries |
| `role` | INTEGER NOT NULL DEFAULT 1 | 0=system, 1=user, 2=assistant, 3=tool, 4=compaction |
| `content` | TEXT | |
| `tool_calls` | TEXT | JSON array, NULL if none |
| `tool_call_id` | TEXT | for role=tool only |
| `tool_name` | TEXT | for role=tool only |
| `is_error` | INTEGER NOT NULL DEFAULT 0 | for role=tool |
| `stop_reason` | INTEGER NOT NULL DEFAULT 0 | see StopReason below |
| `model` | TEXT | which model produced this |
| `usage_in` | INTEGER | input tokens |
| `usage_out` | INTEGER | output tokens |
| `cost_nano` | INTEGER | nanodollars |
| `token_estimate` | INTEGER | chars/4 heuristic |
| `content_bytes` | INTEGER | byte length of content + tool_calls |
| `tool_call_count` | INTEGER NOT NULL DEFAULT 0 | denormalized for plan pass |
| `data` | TEXT | legacy/debug (nullable) |
| `network_hosts` | TEXT | JSON array of hosts the tool run contacted (proxy-observed) |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

### entries.network_hosts

NULL or a JSON array of hostnames the credential-proxy observed during a shell tool execution. Stored for auditing and grant enforcement replay.

### StopReason enum

| Value | Int | Provider sources |
|-------|-----|------------------|
| stop | 1 | `"stop"`, `"end"`, `"end_turn"`, `null` |
| length | 2 | `"length"`, `"max_tokens"` |
| tool_use | 3 | `"tool_calls"`, `"function_call"`, `"tool_use"` |
| error | 4 | `"content_filter"`, `"network_error"`, unknown, missing |
| aborted | 5 | internal — SIGTERM, user cancel, timeout |

### tool_calls JSON format

Provider-neutral; `args` stored as object (not stringified):

```json
[{"id":"tc_1","name":"shell_exec","args":{"cmd":"ls"}}]
```

### FTS5

```sql
CREATE VIRTUAL TABLE entries_fts USING fts5(
  content, content=entries, content_rowid=id,
  tokenize='porter unicode61'
);
```

The AFTER INSERT trigger keys on `entries.type` (not `role`): for `tool_call` and `tool_result` types, the FTS content is prefixed with `tool_name` for better recall. Compaction entries (`type='compaction'`) are excluded from FTS entirely. A separate trigger atomically advances `sessions.leaf_id` on every insert except compaction entries (compaction inserts mid-branch).

### Indexes

```
idx_entries_session       (session_id, id)
idx_entries_parent        (parent_id)
idx_entries_session_role  (session_id, role)
idx_entries_turn          (session_id, turn_id)
idx_entries_turn_type     (session_id, turn_id, type, part_index)
idx_entries_stop_reason   (session_id, stop_reason) WHERE stop_reason != 0
idx_entries_plan          (parent_id, session_id, id, role, stop_reason, token_estimate, tool_call_count)
```

---

## tool_calls

Denormalized tool-call tracking. Links the JSON `tool_calls` array in an assistant entry to the corresponding tool-result entry.

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `session_id` | INTEGER NOT NULL | |
| `entry_id` | INTEGER NOT NULL | the assistant entry containing this call |
| `call_id` | TEXT NOT NULL | matches `tool_calls[].id` in the entry |
| `name` | TEXT NOT NULL | tool name |
| `arguments` | TEXT | JSON |
| `result_entry_id` | INTEGER | links to the tool_result entry |
| `status` | TEXT NOT NULL DEFAULT 'pending' | pending / completed / failed / done |
| `resolved_by` | TEXT | |
| `resolved_at` | INTEGER | |

Indexes: `idx_tool_calls_entry(entry_id)`, `idx_tool_calls_session(session_id, status)`.

---

## tools

Tool registry. Built-in C tools have `builtin=1` and NULL `extension_name`/`path`; JS tools point to a handler file in the shared extension store.

| Column | Type | Notes |
|--------|------|-------|
| `name` | TEXT PRIMARY KEY | |
| `extension_name` | TEXT | NULL for built-in C tools |
| `description` | TEXT | |
| `parameters_json` | TEXT | JSON Schema for arguments |
| `path` | TEXT | handler file in extension store |
| `builtin` | INTEGER NOT NULL DEFAULT 0 | |
| `agent_name` | TEXT | owner scope; NULL = global |
| `enabled` | INTEGER NOT NULL DEFAULT 1 | |
| `policy` | TEXT | JSON restrict-only argument policy |

---

## hooks

Extension-provided hooks. Same provenance model as tools — the DB holds config + a path, never code.

| Column | Type | Notes |
|--------|------|-------|
| `extension_name` | TEXT NOT NULL | |
| `event` | TEXT NOT NULL | one of the six hook lifecycle events |
| `path` | TEXT NOT NULL | handler file in extension store |
| `enabled` | INTEGER NOT NULL DEFAULT 1 | |
| PRIMARY KEY | (extension_name, event, path) | |

---

## hook_directives

Per-request ephemeral directives that cross the main-thread → worker-thread boundary as DB state. Written at dispatch on the poll thread, read by the worker's payload build, deleted at LLM request exit.

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `session_id` | INTEGER NOT NULL | |
| `kind` | TEXT NOT NULL | `inject` or `suppress` |
| `role` | TEXT | inject: `system` or `user` |
| `content` | TEXT | inject payload |
| `entry_id` | INTEGER | suppress target entry |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

Index: `idx_hook_directives_session ON hook_directives(session_id)`.

---

## memory_blocks

Agent-scoped named memory blocks. Each block has a label, char limit, and optional read-only flag.

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `agent_name` | TEXT | NULL = global |
| `label` | TEXT NOT NULL | |
| `value` | TEXT NOT NULL DEFAULT '' | |
| `description` | TEXT | tells agent what block is for |
| `char_limit` | INTEGER NOT NULL DEFAULT 5000 | |
| `read_only` | INTEGER NOT NULL DEFAULT 0 | |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |
| `updated_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |
| UNIQUE | (agent_name, label) | |

---

## memory_entries

Numbered entries within a memory block (the block is the container; entries are the content lines).

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `agent_name` | TEXT NOT NULL | |
| `block_label` | TEXT NOT NULL | |
| `pos` | INTEGER NOT NULL | ordering position |
| `text` | TEXT NOT NULL | |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

Index: `idx_memory_entries_block ON memory_entries(agent_name, block_label, pos)`.

---

## inbox

Durable inbound message queue. Survives crashes; atomic consumption via `BEGIN EXCLUSIVE`.

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `session_id` | INTEGER NOT NULL | |
| `source` | TEXT NOT NULL DEFAULT 'cli' | channel origin |
| `payload` | TEXT NOT NULL | |
| `consumed` | INTEGER NOT NULL DEFAULT 0 | |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

Index: `idx_inbox_pending ON inbox(session_id, consumed) WHERE consumed = 0`.

---

## channel_events

Inbound events from channel processes → daemon. Consumed in FIFO order.

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `channel_name` | TEXT NOT NULL | |
| `event_type` | TEXT NOT NULL DEFAULT 'message' | |
| `payload` | TEXT NOT NULL | JSON |
| `external_id` | TEXT | dedup key |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |
| UNIQUE | (channel_name, external_id) | prevents duplicate events |

---

## channel_outbox

Daemon → channel process delivery queue.

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `channel_name` | TEXT NOT NULL | |
| `session_id` | INTEGER NOT NULL | |
| `payload` | TEXT NOT NULL | JSON |
| `status` | TEXT NOT NULL DEFAULT 'pending' | pending / delivered / failed |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |
| `acked_at` | INTEGER | |

Index: `idx_channel_outbox_pending ON channel_outbox(channel_name, status) WHERE status='pending'`.

---

## cron_jobs

Daemon-scheduled periodic tasks.

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `agent_name` | TEXT | |
| `name` | TEXT NOT NULL | |
| `cron_expr` | TEXT NOT NULL | |
| `session_id` | INTEGER NOT NULL | |
| `task` | TEXT NOT NULL | |
| `enabled` | INTEGER NOT NULL DEFAULT 1 | |
| `next_run_at` | INTEGER NOT NULL DEFAULT 0 | |
| `last_run_at` | INTEGER | |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

---

## processes

Process registry for liveness detection. One row per live cclaw process (daemon or CLI) sharing this DB.

| Column | Type | Notes |
|--------|------|-------|
| `instance_id` | TEXT PRIMARY KEY | unique per process start |
| `pid` | INTEGER NOT NULL | OS pid |
| `mode` | TEXT NOT NULL | `daemon` or `cli` |
| `started_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |
| `heartbeat_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

Sessions stamp `owner_instance` here; owner-scoped recovery reclaims only sessions whose owner is absent (crashed) or stale (no heartbeat within `PROCESS_TTL_SEC`).

---

## Design decisions

1. **Single DB** — all state in one `cclaw.db`; WAL mode allows concurrent readers; writes serialize briefly on commit; one file to back up/move.
2. **DB-driven state machine** — `advance_session()` reads DB state, decides next action; worker threads notify via pipe; no shared-memory IPC.
3. **Owner-scoped recovery** — `sessions.owner_instance` + `processes` table lets peers coexist safely; a crash orphans only that process's sessions.
4. **Split columns over JSON** — SQLite `json_object()` builds wire JSON directly from columns; metadata columns enable plan-pass queries without overflow page loads.
5. **INTEGER ids** — faster joins, smaller indexes, natural ordering.
6. **parent_id tree** — supports branching (Pi model); walk leaf→root via recursive CTE.
7. **Inbox/outbox as tables** — durable queues; survive crashes; atomic consumption.
8. **Agent scoping** — `agent_name` column on sessions, memory_blocks, grants scopes data per-agent within the shared DB.
9. **Grants not config** — typed grant rows with expiry replace the old key-value `agent_config` approach; `approval_mode` + `expires_at` enable fine-grained time-bounded authority.
10. **Extensions as paths** — DB stores definition + path, never code; JS lives in files.
