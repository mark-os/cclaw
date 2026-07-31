# Schema & Data Model

Single SQLite file `cclaw.db` (WAL mode, `busy_timeout` 5000ms). CLI and daemon are peers sharing one source of truth; per-session ownership (`sessions.owner_instance` → `processes`) makes recovery owner-scoped so a live peer's in-flight sessions are never stomped.

Source of truth: `templates/schema.sql` (embedded at build time as `TPL_SCHEMA_SQL`). Current schema version: v39 (`CCLAW_SCHEMA_VERSION` in `src/cclaw.h`); floor v33 (`CCLAW_SCHEMA_MIN` in `src/db.c` — the 2026-07-19 freeze collapsed earlier patch history into it).

## String keys and the agent_name FKs (decided 2026-07-18)

Names, not integer ids, are the join keys across the schema (`agent_name`,
`extension_name`, `channel_name`) — a deliberate choice, revisited and kept
after `agent_rename` was caught orphaning tables: names keep the DB readable
and debuggable, and agents had integer ids once (dropped in the 2026-06-08
redesign) without the ids paying their way. The rule that makes string keys
safe: **an entity that has a rename verb gets real foreign keys; an entity
that is create-and-swap-only doesn't need them.**

Agents are the only entity with a rename verb, so as of v31 every
`agent_name` column (plus `agents.created_by`) is a declared
`REFERENCES agents(name) ON UPDATE CASCADE` FK and `db_open()` sets
`PRAGMA foreign_keys=ON`. Consequences:

- `agent_rename` is one `UPDATE agents SET name=…` — SQLite cascades every
  child table atomically. The old hand-maintained cascade list (which twice
  shipped missing tables) is gone. The only extra statement is
  `config.default_agent`, a value in a key-value table no FK can reach.
- Inserting a child row for a nonexistent agent now fails loudly —
  referential integrity at write time, not just rename time.
- **No `ON DELETE` clause anywhere**: there is no agent-delete verb, so the
  default `NO ACTION` refuses deleting an in-use agent. When a delete verb is
  designed, per-table delete semantics get decided then (note:
  `tools.agent_name` NULL means *global*, so `SET NULL` there would silently
  globalize a dead agent's tools — it must not be the default choice).
- The FK adoption shipped as a new schema floor (v31, delete-and-restart),
  not a `schema_patches[]` migration — SQLite can't `ALTER` an FK into an
  existing table.
- Guard: `test_fk_shape_audit` (test/test_agent_rename.c) fails if any table
  with an `agent_name` column lacks the FK clause; a behavioral seed-and-
  rename audit backs it up.
- Outside the DB, two references stay name-keyed by hand: the workspace dir
  `agents/<name>/` and `config.default_agent` — the rename verb owns both.
- `extension_name`/`channel_name` remain convention (FK-less): no rename
  verbs exist for them. If one grows a rename verb, it inherits this same
  FK treatment first.

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
| `secret` | INTEGER NOT NULL DEFAULT 0 | key resolves env → `secrets` table, never `value` |
| `required` | INTEGER NOT NULL DEFAULT 0 | channel launch gate: must resolve non-empty |

---

## providers

LLM API endpoints. Multiple providers enable fallback routing.

| Column | Type | Notes |
|--------|------|-------|
| `name` | TEXT PRIMARY KEY | |
| `base_url` | TEXT NOT NULL | |
| `endpoint_type` | TEXT NOT NULL DEFAULT 'openai' | openai / gemini |
| `api_key_env` | TEXT NOT NULL DEFAULT '' | env var name holding the key |
| `default_model` | TEXT | |
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
| `context_window` | INTEGER | |
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
| `agent_name` | TEXT NOT NULL | FK → `agents(name)` ON UPDATE CASCADE |
| `recall` | INTEGER NOT NULL DEFAULT 0 | whether to run FTS recall before this request |
| `job_type` | INTEGER NOT NULL DEFAULT 0 | 0 = normal turn, others reserved |
| `status` | TEXT NOT NULL DEFAULT 'pending' | pending / claimed / done |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

Index: `idx_llm_jobs_pending ON llm_jobs(status) WHERE status='pending'`.

---

## llm_responses

Raw LLM response archive for forensics and debugging. One row per HTTP response (success or failure).

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `session_id` | INTEGER NOT NULL | |
| `iteration_id` | INTEGER NOT NULL | the `entries.iteration_id` this request belongs to |
| `model` | TEXT | |
| `status` | TEXT NOT NULL | ok / empty / malformed / http_<code> / timeout / network_error |
| `provider_id` | TEXT | provider's own response id (`$.id`), NULL if absent |
| `body` | BLOB | JSONB when parseable, raw text otherwise |
| `request_body` | BLOB | JSONB of sent payload (failures only); NULL on success |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

Retention controlled by config key `llm_response_archive_max`: >0 keeps the most
recent N 'ok' rows **plus** the most recent N failures (so `[resp #N]` citations in
error entries outlive routine traffic), 0 disables, <0 keeps all.

Read rows with `cclaw resp [<id> [req] | list [n]]` — bodies are JSONB, which
system sqlite3 CLIs older than 3.45 (Debian bookworm ships 3.40) cannot decode;
the vendored SQLite in the binary is the guaranteed reader.

Index: `idx_llm_responses_iteration ON llm_responses(session_id, iteration_id)`.

---

## extensions

Shared extension registry. Each extension is a directory in `~/.cclaw/extensions/<name>` containing JS files for tools, hooks, and channels.

| Column | Type | Notes |
|--------|------|-------|
| `name` | TEXT PRIMARY KEY | |
| `path` | TEXT NOT NULL | directory path |
| `version` | TEXT DEFAULT '0.0.0' | |
| `owner_agent` | TEXT | who promoted it ('system' for extensions shipped in the binary) |
| `published` | INTEGER NOT NULL DEFAULT 0 | single publish flag |
| `enabled` | INTEGER NOT NULL DEFAULT 1 | |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

---

## agents

Agent identity and per-agent config. Name is the primary key — no integer id.

| Column | Type | Notes |
|--------|------|-------|
| `name` | TEXT PRIMARY KEY | |
| `primary_model` | TEXT | model override; NULL uses config `default_model` |
| `secondary_model` | TEXT | fallback model; NULL uses config `default_secondary_model` |
| `system_prompt` | TEXT | inline prompt override |
| `description` | TEXT | |
| `max_iterations` | INTEGER DEFAULT 25 | tool loop cap per turn |
| `max_output_tokens` | INTEGER | per-request cap |
| `shell_timeout` | INTEGER DEFAULT 30 | seconds |
| `shell_path` | TEXT | interpreter for shell_exec's `-c`; NULL = `/bin/sh` |
| `sandbox_profile` | TEXT DEFAULT 'standard' | host / standard / restricted |
| `created_by` | TEXT | FK → `agents(name)` ON UPDATE CASCADE; creating agent (`update_agent` authorization); NULL = operator |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

---

## grants

Normalized capability grants for agents. Replaces the old `agent_config` key-value approach with typed rows.

| Column | Type | Notes |
|--------|------|-------|
| `agent_name` | TEXT NOT NULL | FK → `agents(name)` ON UPDATE CASCADE |
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

Secret store (specs/security.md): the single home for every encrypted value, all AEAD-sealed with the master key. `scope` splits two audiences: `agent` secrets feed the per-call snapshot (`secrets_snapshot()` merges them with the env-collected `CCLAW_SECRET_*` base — env wins on a name collision) and are usable via `{{SECRET:name}}`; `system` secrets are provider API keys consumed by the daemon (`db_secret_get_system()`) and are **excluded from the snapshot**, so an agent can never interpolate them. Enforcement is `secret_hosts` having zero rows for the name (fail-closed, same as any other secret).

| Column | Type | Notes |
|--------|------|-------|
| `name` | TEXT PRIMARY KEY | `^[A-Z][A-Z0-9_]*$` — becomes `CCLAW_SECRET_<name>` / `{{SECRET:name}}` |
| `value` | TEXT NOT NULL | `enc:<hex(...)>`, never plaintext |
| `source` | TEXT NOT NULL DEFAULT 'operator' | `operator` (`cclaw secret set`) \| `generated` (`secret_create` tool) \| `captured` (`save_secret` on a tool call) |
| `scope` | TEXT NOT NULL DEFAULT 'agent' | `agent` (interpolatable/injectable) \| `system` (provider keys, daemon-only) |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

Written by: `cclaw secret set\|rm\|list` (operator verb), the `secret_create` tool, `admin_set_key` (scope `system`), and `secret_capture_apply` (`save_secret` on web_fetch/shell_exec/js_eval). The DLP scanner never writes here — it redacts. (A `status` column marked scanner-quarantined rows until schema v20 removed quarantine.)

---

## agent_extensions

Links agents to their enabled extensions.

| Column | Type | Notes |
|--------|------|-------|
| `agent_name` | TEXT | FK → `agents(name)` ON UPDATE CASCADE; NULL = global |
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
| `prev_extension_name` | TEXT | revert target recorded by channel swap |
| `type` | TEXT NOT NULL DEFAULT '' | telegram / webhook / custom |
| `binary_path` | TEXT NOT NULL DEFAULT '' | path to channel process |
| `status` | TEXT NOT NULL DEFAULT 'draft' | see lifecycle below |
| `pid` | INTEGER | daemon tracks the running process |
| `default_agent` | TEXT | FK → `agents(name)`. Open-door policy: non-NULL = unrouted chats are accepted and get a new session for this agent; NULL = fail-closed |
| `default_tool_filter` | TEXT | JSON array of tool names; NULL = unrestricted. Frozen onto every session the gate creates for an *unrouted* chat (open door **and** admin fallback). Set by `route add <ch> '*' <agent> --tools ...` |
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

Pins a chat (platform conversation id) to a session; the session names its
agent (v33 route-model unification). Routes never name an agent — the
channel-wide default lives on `channels.default_agent`.

| Column | Type | Notes |
|--------|------|-------|
| `channel_name` | TEXT NOT NULL | |
| `chat_id` | TEXT NOT NULL | Telegram chat id, Discord channel/DM id, … |
| `session_id` | INTEGER NOT NULL | FK → `sessions(id)`; the pin IS the binding |
| `delivery_mode` | TEXT NOT NULL DEFAULT 'auto' | `auto` = turn output auto-delivers to the origin chat; `explicit` = only `channel_send` |
| `tool_filter` | TEXT | JSON array of tool names; NULL = unrestricted. Frozen onto the pinned session at its creation (`route add --tools`); later route edits don't retro-apply |
| `system_prompt_suffix` | TEXT | Appended to the pinned session's system prompt every turn (`route add --prompt`); NULL = nothing appended. Read live — unlike `tool_filter`, an edit takes effect on the next turn |
| PRIMARY KEY | (channel_name, chat_id) | |

---

## sessions

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `name` | TEXT | human label |
| `agent_name` | TEXT | FK → `agents(name)` ON UPDATE CASCADE; scopes session to agent |
| `channel_name` | TEXT | originating channel |
| `chat_id` | TEXT | originating chat id |
| `chat_title` | TEXT | display name of the bound chat as observed by the channel runner (`#general @ My Server`, `DM with alice`); injected into the system prompt so the bot knows where it is chatting. Display only — sends always address by `chat_id`. NULL until a runner reports a name |
| `parent_session_id` | INTEGER DEFAULT -1 | sub-agent parent (-1 = top-level) |
| `parent_tool_call_id` | TEXT | the tool_call that spawned this sub-session |
| `depth` | INTEGER NOT NULL DEFAULT 0 | sub-agent nesting depth |
| `tool_filter` | TEXT | JSON array of tool names (see below) |
| `state` | TEXT NOT NULL DEFAULT 'idle' | see state machine below |
| `owner_instance` | TEXT | FK to `processes.instance_id`; NULL ⟺ idle |
| `turn_iteration` | INTEGER NOT NULL DEFAULT 0 | iteration within current turn |
| `turn_context` | TEXT | `<RELEVANT_CONTEXT>` block, materialized once at turn start (`llm_proc.c`) and reused verbatim by every tool-loop iteration so the request prefix stays byte-stable for prompt caching; NULL = no block |
| `leaf_id` | INTEGER DEFAULT -1 | current branch tip entry |
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
| `turn_id` | INTEGER | the turn this entry belongs to — minted when a user entry follows a non-user one, inherited from the parent thereafter (trigger `entries_turn_ai`). The context cut and compaction snap to it so neither splits a turn. |
| `iteration_id` | INTEGER | one LLM request/response *inside* a turn (`db_next_iteration_id`); shared by the assistant message, its tool_call parts and their tool results |
| `type` | TEXT NOT NULL DEFAULT 'user_message' | entry type discriminator |
| `part_index` | INTEGER NOT NULL DEFAULT 0 | ordering within multi-part entries |
| `role` | INTEGER NOT NULL DEFAULT 1 | 0=system, 1=user, 2=assistant, 3=tool, 4=compaction |
| `content` | TEXT | see below |
| `tool_calls` | TEXT | JSON array, NULL if none (legacy) |
| `tool_call_id` | TEXT | for type=tool_call and tool_result |
| `tool_name` | TEXT | for type=tool_call and tool_result |
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

### entries.content by type

| type | content holds |
|------|---------------|
| `system` | System prompt text |
| `user_message` | User's message text |
| `assistant_message` | Assistant's text response (may be NULL if tool_calls only) |
| `tool_call` | **Validated JSON arguments** (normalized via `json()` at ingest; single source of truth for tool call args) |
| `tool_result` | Tool execution output (text) |
| `reasoning` | Model reasoning/thinking text |
| `compaction` | Compressed summary text |

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
idx_entries_session    (session_id, id)                         -- per-session scans in entry order
idx_entries_iteration_type (session_id, iteration_id, type, part_index) -- covering: payload builder's sibling-part lookup
idx_entries_created    (created_at)                             -- budget gate time-window sums
```

One index per access shape, nothing speculative (schema v27 dropped four
others). Branch walks (leaf→root recursive CTEs) join on `e.id` — rowid
seeks on the table b-tree, no index needed. Narrower per-session filters
(`role=`, `stop_reason=`) seek `idx_entries_session` and read a few rows.

---

## tool_calls

Workflow state table for tool-call tracking. Links a `type='tool_call'` entry to its result and tracks execution status. Arguments live in `entries.content` (validated JSON, single source of truth) — this table holds no data copy.

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `session_id` | INTEGER NOT NULL | |
| `entry_id` | INTEGER NOT NULL | the tool_call entry (args in its `content`) |
| `call_id` | TEXT NOT NULL | provider's tool_call_id |
| `name` | TEXT NOT NULL | tool name (denormalized for fast queries) |
| `result_entry_id` | INTEGER | links to the tool_result entry |
| `status` | TEXT NOT NULL DEFAULT 'pending' | pending / running / done / awaiting_approval |
| `resolved_by` | TEXT | |
| `resolved_at` | INTEGER | |

Indexes: `idx_tool_calls_entry(entry_id)`, `idx_tool_calls_session(session_id, status)`.

---

## tools

Tool registry. Built-in C tools have NULL `extension_name`/`path`; JS tools point to a handler file in the shared extension store.

| Column | Type | Notes |
|--------|------|-------|
| `name` | TEXT PRIMARY KEY | |
| `extension_name` | TEXT | NULL for built-in C tools |
| `description` | TEXT | |
| `parameters_json` | TEXT | JSON Schema for arguments |
| `path` | TEXT | handler file in extension store |
| `agent_name` | TEXT | FK → `agents(name)` ON UPDATE CASCADE; owner scope; NULL = global |
| `enabled` | INTEGER NOT NULL DEFAULT 1 | |
| `policy` | TEXT | JSON restrict-only argument policy |
| `egress_hosts` | TEXT | comma-separated hosts declared by the manifest; non-NULL **replaces** the agent's host grants for calls of this tool. Only a promoted extension tool can carry one — see [extensions.md](extensions.md#declared-reach-hosts) |

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

Agent-scoped named memory blocks. A block is a pure **container**: it carries a label, char limit, and optional read-only flag, but no text of its own — its content is the numbered `memory_entries` rows that name it. (v39 dropped the scalar `value` column; nothing rendered it, so text stored there was invisible to the model. The migration rescued any non-empty value as entry 1 of blocks that had no entries yet.)

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `agent_name` | TEXT | FK → `agents(name)` ON UPDATE CASCADE; NULL = global |
| `label` | TEXT NOT NULL | |
| `description` | TEXT | tells agent what block is for |
| `char_limit` | INTEGER NOT NULL DEFAULT 5000 | |
| `read_only` | INTEGER NOT NULL DEFAULT 0 | |
| `placement` | TEXT NOT NULL DEFAULT 'system' | `system` = rendered into the system prompt once per session (`agent_config.c`); `context` = rendered into the `<RELEVANT_CONTEXT>` block at each turn start. New blocks default to `context` via `memory_block_create` (the API default every insert path goes through — the column DEFAULT is inert); only the Assistant's seeded AGENT/USER identity blocks ask for `system` |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |
| `updated_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |
| UNIQUE | (agent_name, label) | |

---

## memory_entries

Numbered entries within a memory block (the block is the container; entries are the content lines).

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `agent_name` | TEXT NOT NULL | FK → `agents(name)` ON UPDATE CASCADE |
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
| `attempts` | INTEGER NOT NULL DEFAULT 0 | delivery retry counter |
| `next_attempt_at` | INTEGER NOT NULL DEFAULT 0 | retry backoff gate |
| `deliver_mode` | INTEGER NOT NULL DEFAULT 0 | degradation ladder: 0 = formatted (platform rich text), 1 = plain (see below) |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |
| `acked_at` | INTEGER | |

Delivery degradation ladder (`deliver_mode`): on a terminal 4xx for a formatted row the C loop re-delivers once at plain — platform-agnostic, it never inspects *why* the format was rejected. The ladder only descends, so re-delivery can't loop.

Index: `idx_channel_outbox_pending ON channel_outbox(channel_name, status) WHERE status='pending'`.

---

## media_jobs

Inbound media awaiting capability-routed preprocessing (e.g. voice → transcript). Transient by design: a worker turns the row into a text-only `inbox` row and deletes it. Rows only survive a crash (resubmitted at daemon start); `attempts` caps crash-loop retries.

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `session_id` | INTEGER NOT NULL | |
| `source` | TEXT NOT NULL DEFAULT '' | channel origin |
| `payload` | TEXT NOT NULL | full channel-emitted message JSON including `media.data_b64` |
| `attempts` | INTEGER NOT NULL DEFAULT 0 | caps crash-loop retries |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

---

## cron_jobs

The one scheduler: every future wake — recurring task, one-shot commitment,
or the agent-level heartbeat pulse — is a durable row here, fired by
`run_due_jobs()` off the daemon's existing 30s `db_periodic` tick. There is
no separate heartbeat thread; heartbeat is a seeded row with `kind='heartbeat'`.

A job's schedule is exactly one of `cron_expr` (recurring, 5-field),
`run_at` (one-shot unix timestamp), or `interval_s` (fixed period in
seconds) — the unused fields are `''` / NULL / NULL respectively
(`cron_list()` reads the NULLs back as 0). `kind` is orthogonal to
schedule and controls payload/targeting.

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `agent_name` | TEXT | FK → `agents(name)` ON UPDATE CASCADE |
| `name` | TEXT NOT NULL | |
| `cron_expr` | TEXT NOT NULL | `''` sentinel when `run_at` or `interval_s` is used instead (kept NOT NULL — SQLite can't relax it without a table rebuild) |
| `run_at` | INTEGER | one-shot fire time; row is deleted (not rescheduled) once fired |
| `interval_s` | INTEGER | fixed-period cadence; not exposed on the model-facing `cron_set` schema today (heartbeat rows use it internally) |
| `kind` | TEXT NOT NULL DEFAULT 'task' | `'task'` or `'heartbeat'` |
| `session_id` | INTEGER NOT NULL | `0` = resolve to the agent's most recently active session at fire time (`kind='task'`) or most recently active *idle* session (`kind='heartbeat'`, which ignores this column entirely) |
| `task` | TEXT NOT NULL | injected message; unused for `kind='heartbeat'` (fires the constant `HEARTBEAT_PROMPT` instead) |
| `enabled` | INTEGER NOT NULL DEFAULT 1 | |
| `next_run_at` | INTEGER NOT NULL DEFAULT 0 | |
| `last_run_at` | INTEGER | |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

Firing rules (`src/cron.c`):
- `kind='task'`, one-shot (`run_at`): inject `task` into the resolved session, then **delete the row**.
- `kind='task'`, interval or recurring: inject `task`, reschedule `next_run_at`. A recurring job whose expression fails to parse at reschedule time is disabled, never retried hourly.
- `kind='heartbeat'`: skip silently if the agent has no idle session, or if that session already has an unconsumed `source='heartbeat'` inbox row (never stack pulses).

Guardrails, both in the `config` registry: `cron_min_interval_seconds`
(default 300, enforced in `cron_add()` fire-to-fire — not just at the
`cron_set` tool boundary, since `extension_promote` is a second
model-reachable caller) and `cron_max_jobs_per_session` (default 10,
enforced in the `cron_set` tool handler only — manifest/heartbeat rows use
`session_id=0` and are legitimately operator-shaped).

Every agent gets a seeded, **disabled** heartbeat row (`name='heartbeat'`,
`interval_s=1800`) at creation — `cron_seed_heartbeat()`, called from both
`agent_definition_apply` and the daemon's `ensure_default_agent` — and, for
pre-existing agents, via the (since-retired) v23 schema patch. Enabling it is a deliberate
operator/agent act — heartbeats cost an LLM call per fire.

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
