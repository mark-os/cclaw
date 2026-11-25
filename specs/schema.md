# Schema & Data Model

Single SQLite file `cclaw.db` (WAL mode). A custom busy handler (`src/db.c`) replaces `busy_timeout`: it retries with progressive backoff for about 4.3 s and logs contention, then returns `SQLITE_BUSY`. CLI and daemon are peers that share one source of truth. Per-session ownership (`sessions.owner_instance` → `processes`) makes recovery owner-scoped, so recovery never resets the in-flight sessions of a live peer.

Source of truth: `templates/schema.sql` (embedded at build time as `TPL_SCHEMA_SQL`). Current schema version: v71 (`CCLAW_SCHEMA_VERSION` in `src/cclaw.h`). Floor: v70 (`CCLAW_SCHEMA_MIN`, also in `src/cclaw.h`). The 2026-11-24 freeze at v0.8.0's schema deleted earlier patch history; an older DB hops through v0.8.0, which patches it to v70.

**Patch rule — a new column whose NULL triggers action must backfill in the same patch.** If NULL means "something is owed" to any reader (a sweep, a retry, a guard), every pre-existing row satisfies that predicate the moment the column lands, and the first post-deploy tick acts on all of history at once. The `ALTER TABLE … ADD COLUMN` and the `UPDATE` that stamps old rows to the no-action value belong in one patch entry (precedent: v41's `parent_notified_at` backfill — without it the convergence sweep would have re-notified every terminal child ever recorded).

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
- Configuration-side links are declared FKs too (v59,
  plan/projects/v0.8-data-and-explorer.md decision 19), all `ON UPDATE
  CASCADE` with no `ON DELETE`: `models.provider_name` → `providers`;
  `tools`/`hooks`/`agent_extensions.extension_name` and
  `channels.extension_name`/`prev_extension_name` → `extensions`;
  `extensions.owner_agent` → `agents`; `channel_state.channel_name` →
  `channels`; `approval_decisions.approval_id` → `approvals`. No rename verb
  needs them; they exist so a dangling write fails at write time and so the
  links can be read from `pragma_foreign_key_list`. Guard:
  `test_config_link_audit` (test/test_agent_rename.c).
- `grants.approval_id` → `approvals(id)` (v60): the approval being applied
  when the grant was written.
- Chats (v61, decisions 1–10): `channel_chats.channel_name` → `channels`
  (ON UPDATE CASCADE), `channel_chats.session_id` → `sessions(id)` (the pin),
  and the composite `sessions(channel_name, chat_id)` and
  `cron_jobs(channel_name, chat_id)` → `channel_chats` (ON UPDATE CASCADE;
  both columns NULL = no chat, and a row with only one of them is not
  checked). A chat row is never deleted.
- Not declared, on purpose: `secret_hosts.secret_name` and a `secret`
  grant's `value` (a `CCLAW_SECRET_*` environment secret has bindings and
  grants but no `secrets` row — `db_secret_rm` removes all three in code), and the
  conversation tables' `session_id`/`channel_name` columns and `-1`
  sentinels (entries, tool_calls, llm_responses, inbox, channel_outbox,
  channel_events) — parked until a later decision.

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

LLM API endpoints. Multiple providers enable fallback routing. Health is
tracked per model (`models.status`), never per provider (the never-read
`providers.status` column was dropped in v59). A provider that still has
models cannot be deleted (`models.provider_name` FK).

| Column | Type | Notes |
|--------|------|-------|
| `name` | TEXT PRIMARY KEY | |
| `base_url` | TEXT NOT NULL | |
| `endpoint_type` | TEXT NOT NULL DEFAULT 'openai' | openai / gemini / anthropic |
| `api_key_env` | TEXT NOT NULL DEFAULT '' | env var name holding the key |
| `default_model` | TEXT | fresh-install seed sugar only — models are registered in `models` |
| `priority` | INTEGER NOT NULL DEFAULT 0 | lower = preferred |
| `request_extra` | TEXT | JSON merged (`json_patch`, RFC 7386) into every outgoing request body for this provider, last — so it overrides anything cclaw built. NULL = no-op. Seeded on `openrouter` with `{"provider":{"ignore":["StreamLake"]}}` |

---

## models

Pure catalog + health stats. Routing order lives in `agent_models` — nothing
here orders candidates.

| Column | Type | Notes |
|--------|------|-------|
| `id` | TEXT PRIMARY KEY | canonical routing key; new registrations are `model@provider` |
| `provider_name` | TEXT NOT NULL | FK → `providers(name)` ON UPDATE CASCADE (v59) |
| `model` | TEXT NOT NULL | wire model id sent to the provider |
| `context_window` | INTEGER | |
| `max_output_tokens` | INTEGER | |
| `capabilities` | TEXT DEFAULT '[]' | JSON array of capability tags |
| `effort_map` | TEXT | JSON: how this model spells reasoning effort on the wire — `{"format": "openrouter\|openai\|deepseek\|gemini-level\|gemini-budget\|anthropic", "levels": {level: wire value}}`. A missing/null level is unsupported and clamps to the nearest supported one. NULL = the endpoint default. See [providers.md](providers.md#reasoning-effort) |
| `status` | TEXT NOT NULL DEFAULT 'healthy' | healthy / degraded / disabled |
| `degraded_until` | INTEGER | unixepoch cooldown; NULL while degraded = config-degraded (missing key) |
| `total_requests` | INTEGER DEFAULT 0 | lifetime counter |
| `total_tokens_in` | INTEGER DEFAULT 0 | |
| `total_tokens_out` | INTEGER DEFAULT 0 | |
| `total_cost_nano` | INTEGER DEFAULT 0 | nanodollars |
| `consec_failures` | INTEGER DEFAULT 0 | consecutive transient failures; any success resets; >= `health_fail_threshold` stamps `degraded_until` (re-stamped on every further crossing) |
| `last_success_at` | INTEGER | |
| `last_error_at` | INTEGER | |
| `synced_at` | INTEGER | last remote metadata sync |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

---

## agent_models

The whole routing policy: an agent's
candidates in `pos` order, nothing appended after the list. Health reorders it
(degraded rows sink at selection), never empties it. `models` rows referenced
here can't be deleted — fix lists first; `disabled` is the soft option.

One ordered list per `purpose` (v59): `chat` drives the agent's turns (every
list that existed before v59 became `chat`); `image` and `audio` choose the
model that turns inbound media into text for the agent's sessions
(`model_pick_for_media`, `src/llm_proc.c`). An agent with no rows for a media
purpose uses the global capability pick (`model_pick_by_capability`). With
rows, that list is the policy exactly as for chat: pos order, a model in its
error cooldown sinks below healthy ones, `disabled` drops out, nothing global
is appended. Triggers `agent_models_capability_bi`/`_bu` refuse an `image` or
`audio` row (insert, or update of `purpose`/`model_id`) whose model's
`capabilities` array does not contain that purpose — one check every write
path goes through. Removing a capability from a model that is already
assigned is not checked.

`utility` (v65) is the list a script's `LLM(prompt, {purpose: "utility"})`
walks ([js-llm.md](js-llm.md#purpose)) — side work such as a `Stop`
hook's review, which wants a cheap, fast model rather than the one that
drives the turns. Any model may be on it (no capability is required) and an
entry may carry a `reasoning_effort`. An agent with no `utility` rows is
served from its `chat` list. The v65 patch rebuilds the table to widen the
CHECK; rows are carried unchanged.

| Column | Type | Notes |
|--------|------|-------|
| `agent_name` | TEXT NOT NULL | FK agents(name), ON UPDATE CASCADE ON DELETE CASCADE |
| `purpose` | TEXT NOT NULL DEFAULT 'chat' | `chat\|image\|audio\|utility` (CHECK-enforced) |
| `model_id` | TEXT NOT NULL | FK models(id) |
| `pos` | INTEGER NOT NULL | 0 = first; PK (agent_name, purpose, pos) |
| `reasoning_effort` | TEXT | `off\|minimal\|low\|medium\|high` (CHECK-enforced), or NULL = send nothing. Translated per model through `models.effort_map` at request-build time — see [providers.md](providers.md#reasoning-effort) |
| `trim_threshold` | INTEGER | v68. Tokens; CHECK > 0. Over it at the end of an iteration, the session fires `ContextThreshold` and, if still over, C trims it. NULL = half the model's `context_window`. Read off the governing entry only — the agent's first `chat` entry whose model is not `disabled` (`context_limits`) — see [memory.md](memory.md#the-thresholds) |
| `prune_target` | INTEGER | v68. Tokens; CHECK > 0 and below `trim_threshold` when that is set. How far C's trim cuts. NULL = half the threshold |
| `cache_lifetime` | INTEGER | v68. Seconds the provider keeps this model's prompt cache; CHECK > 0. Arms `CacheExpiring` ([hooks.md](hooks.md#cacheexpiring)); NULL = unknown, no event. On an Anthropic-format model it is 300 or 3600 and selects the cache marker sent ([providers.md](providers.md#anthropic-messages)) |
| `reasoning_display` | TEXT NOT NULL DEFAULT 'summary' | v68. `summary\|none` (CHECK-enforced): whether the model is asked for a readable summary of its reasoning, kept as the `reasoning` entry's text. Mapped per wire format — see [providers.md](providers.md#reasoning-display) |

The four v68 columns apply to the chat list (the explorer and the config
document refuse them elsewhere; the schema does not). Triggers:

- `agent_models_window_bi` / `_bu` (insert; update of `trim_threshold` or
  `model_id`) refuse a `trim_threshold` larger than the model's
  `context_window`.
- `agent_models_lifetime_bi` / `_bu` (insert; update of `cache_lifetime` or
  `model_id`) refuse a `cache_lifetime` other than 300 or 3600 on a model
  whose provider's `endpoint_type` is `anthropic`.
- `agent_models_lifetime_ai` sets `cache_lifetime` = 3600 on a new `chat` row
  for an Anthropic model that gave none. NULL is reachable only by clearing it
  afterwards, and a write path that replaces the list (delete + insert) puts
  3600 back.

The v68 patch sets `cache_lifetime` = 3600 on every existing Anthropic `chat`
row and leaves every other row's new columns at their defaults.

---

## models_cache

Availability-probe cache (v45, config-ax Phase 3): what a provider's
`/models` endpoint *lists*, as opposed to what `models` *registers* for
routing. Filled lazily by `search_models` / `cclaw models` on a 12h TTL
(`MODELS_CACHE_TTL_S`); never probed at startup or per turn. Slim columns
on purpose — description/provider blobs from aggregator catalogs must
never reach a context window. A listing is advisory; only registered ids
route.

| Column | Type | Notes |
|--------|------|-------|
| `provider_name` | TEXT NOT NULL | PK part; FK-ish to `providers.name` |
| `id` | TEXT NOT NULL | PK part; the id as the catalog lists it |
| `context_length` | INTEGER | |
| `prompt_price` | TEXT | per-token price string as published |
| `completion_price` | TEXT | |
| `modality` | TEXT | e.g. `text->text` |
| `synced_at` | INTEGER NOT NULL | unixepoch of the refresh; drives the TTL |

---

## llm_jobs

Transient work queue. The daemon poll loop writes one row per LLM request. A worker thread runs the request and deletes the row when it completes (`src/llm_worker.c`). A row that exists means a worker holds or will hold that session.

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `session_id` | INTEGER NOT NULL | |
| `agent_name` | TEXT NOT NULL | FK → `agents(name)` ON UPDATE CASCADE |
| `recall` | INTEGER NOT NULL DEFAULT 0 | whether to run FTS recall before this request |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

The row's existence is its state (completion deletes it); the always-`pending`
`status` column and its index were dropped in v59; `job_type` (turn or
compaction) in v68, with the compaction it named.

---

## llm_responses

Raw LLM response archive for forensics and debugging. One row per HTTP response (success or failure).

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `session_id` | INTEGER NOT NULL | |
| `iteration_id` | INTEGER NOT NULL | the `entries.iteration_id` this request belongs to |
| `model` | TEXT | |
| `status` | TEXT NOT NULL | ok / empty / malformed / tool_markup / ingest_error / http_<code> / timeout / network_error |
| `provider_id` | TEXT | provider's own response id (`$.id`), NULL if absent |
| `upstream_provider` | TEXT | upstream backend that served it (`$.provider`, e.g. OpenRouter's `StreamLake`), NULL if absent — a different field from `provider_id` |
| `body` | BLOB | JSONB when parseable, raw text otherwise |
| `request_body` | BLOB | The payload sent, as the JSON text that went on the wire (JSONB on rows written before v0.8), on every archived row (ok and failure); subject to the same retention pruning |
| `cached_tokens` | INTEGER | prompt tokens served from cache; NULL when unreported |
| `cache_write_tokens` | INTEGER | prompt tokens written to cache; NULL when unreported |
| `reasoning_tokens` | INTEGER | subset of completion tokens spent thinking |
| `cost` | REAL | provider-reported cost in dollars; NULL when unreported |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

**Usage semantics.** `prompt_tokens` (stored on `entries.usage_in`) is the
wire's **cache-inclusive** input total on every provider currently supported —
OpenAI-compat (OpenRouter, DeepSeek), and Gemini-native — and `cached_tokens` is
a **subset** of it, never an addend. Reported spellings, all COALESCEd at
ingestion: `usage.prompt_tokens_details.cached_tokens` /
`usage.prompt_cache_hit_tokens` (DeepSeek-direct) /
`usageMetadata.cachedContentTokenCount` (Gemini). Anthropic-native is
**cache-exclusive** on the wire (`cache_read_input_tokens` and
`cache_creation_input_tokens` are *additional* to `input_tokens`), so ingestion
stores their sum and the convention holds there too. Gemini completion tokens are stored as
`candidatesTokenCount + thoughtsTokenCount` (thoughts is a sibling, not
included in candidates).

No consumer may *depend* on any of these fields: absent or zero must degrade to
the pre-existing behavior exactly. Consumers: `rate_limit_check`, which
weights cache reads at 0.25 via `COALESCE(cached_tokens,0)`; the
`CacheExpiring` payload (`cache_read_tokens` / `cache_write_tokens` of the
session's last `ok` row); and the explorer's cache view, which reads
`entries.cached_tokens`.

Retention controlled by config key `llm_response_archive_max`: >0 keeps the most
recent N 'ok' rows **plus** the most recent N failures (so `[resp #N]` citations in
error entries outlive routine traffic), 0 disables, <0 keeps all.

Read rows with `cclaw resp [<id> [req] | list [n]]` — bodies are JSONB, which
system sqlite3 CLIs older than 3.45 (Debian bookworm ships 3.40) cannot decode;
the vendored SQLite in the binary is the guaranteed reader.

Index: `idx_llm_responses_iteration ON llm_responses(session_id, iteration_id)`.

---

## extensions

Shared extension registry. Each extension is a directory in the shared store `~/.cclaw/extensions/<name>` that holds the manifest and the JS files for its tools, hooks, channels and scripts.

| Column | Type | Notes |
|--------|------|-------|
| `name` | TEXT PRIMARY KEY | |
| `path` | TEXT NOT NULL | shared store directory |
| `version` | TEXT DEFAULT '0.0.0' | |
| `owner_agent` | TEXT | FK → `agents(name)` ON UPDATE CASCADE (v59): the agent that promoted it; NULL = shipped in the binary (was the string `'system'` before v59) |
| `published` | INTEGER NOT NULL DEFAULT 0 | single publish flag |
| `enabled` | INTEGER NOT NULL DEFAULT 1 | |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

---

## containments

How an agent's tool children are contained ([containment.md](containment.md)).
The shipped rows (`standard`, `offline`, `basic-firewalled`, `basic`, `none`) are restored to
their shipped values at every start (`CONTAIN_SHIPPED`, `src/sandbox.c`);
rows an operator adds are left alone. No agent-facing verb writes it.
`agents.containment` names a row; a name with no row refuses that agent's
tool calls.

| Column | Type | Notes |
|--------|------|-------|
| `name` | TEXT PRIMARY KEY | |
| `files` | TEXT NOT NULL | `private` \| `confined` \| `open` (CHECK) |
| `network` | TEXT NOT NULL | `none` \| `firewalled` \| `advisory` \| `direct` (CHECK) |
| `nproc` | INTEGER NOT NULL DEFAULT 0 | `RLIMIT_NPROC`; 0 = no cap |
| `cpu_sec` | INTEGER NOT NULL DEFAULT 0 | `RLIMIT_CPU` seconds; 0 = no cap |

---

## agents

Agent identity and per-agent config. Name is the primary key — no integer id.

| Column | Type | Notes |
|--------|------|-------|
| `name` | TEXT PRIMARY KEY | |
| `system_prompt` | TEXT | inline prompt override |
| `description` | TEXT | |
| `max_iterations` | INTEGER DEFAULT 25 | tool loop cap per turn |
| `max_output_tokens` | INTEGER | per-request cap |
| `shell_timeout` | INTEGER DEFAULT 30 | seconds |
| `shell_path` | TEXT | interpreter for shell_exec's `-c`; NULL = `/bin/sh` |
| `containment` | TEXT DEFAULT 'standard' | a `containments` row by name; no row = tool calls refused ([containment.md](containment.md)) |
| `created_by` | TEXT | FK → `agents(name)` ON UPDATE CASCADE; creating agent (`update_agent` authorization); NULL = operator |
| `hold_until` | INTEGER | quiesce lease deadline (`cclaw rename-agent`); while in the future no new turn opens for this agent. NULL = no hold; expiry self-heals |
| `hold_holder` | TEXT | who holds the lease (`cli:<pid>`) |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

---

## grants

Normalized capability grants for agents, one typed row per capability.

| Column | Type | Notes |
|--------|------|-------|
| `agent_name` | TEXT NOT NULL | FK → `agents(name)` ON UPDATE CASCADE |
| `kind` | TEXT NOT NULL | capability axis: `tool`, `host`, `read_path`, `write_path`, `secret` (v60) |
| `value` | TEXT NOT NULL | the granted name/pattern; for `secret`, the secret's name |
| `approval_mode` | TEXT NOT NULL DEFAULT 'silent' | only for kind='tool' (see below) |
| `expires_at` | INTEGER | unixepoch; NULL = permanent. Row is dead after this time. |
| `created_at` | INTEGER DEFAULT (unixepoch()) | |
| `source` | TEXT NOT NULL | how the row came to exist (v60), CHECK-enforced, **no default** — a writer that does not say is refused: `approval` (an approved request was applied: request_config, update_agent, "always allow", grant-from-history), `operator` (CLI / dashboard), `agent_create` (given to a new agent at definition: baseline, declared, clone copy), `extension` (a promoted extension's tools, to its owner), `seed` (startup bootstrap, on-disk / built-in agent definitions), `capture` (the agent captured or minted this secret), `migration` (a schema patch — v60's secret grants), `legacy` (existed before v60; origin unknown). A re-grant that revives an expired row takes the new origin; later edits are in `change_log` |
| `approval_id` | INTEGER | FK → `approvals(id)` (v60): the approval being applied when the row was written; NULL when none |
| PRIMARY KEY | (agent_name, kind, value) | |

A `secret` grant is what lets an agent have that secret interpolated into, or
delivered to, its tool calls (specs/security.md "Using a secret is a grant").
v60 granted every existing agent every secret it could use before (source
`migration`).

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
| `tool_name` | TEXT | WHAT parked — the tool that asked. The sole dispatch/dedup key |
| `park_reason` | TEXT | WHY it parked: `approval_required` or `sensitive_target` |
| `args_json` | TEXT | |
| `resolve` | TEXT NOT NULL DEFAULT 'rerun' | `rerun` = re-issue the frozen tool call on approval; `apply` = apply a capability grant (request_config) |
| `state` | TEXT NOT NULL DEFAULT 'pending' | pending / approved / denied / consumed (`approval_consume`: an approved once-ticket after its frozen call ran) |
| `decided_via` | TEXT | who/what resolved it |
| `requested_at` | INTEGER DEFAULT (unixepoch()) | |
| `expires_at` | INTEGER | park deadline; fail-closed deny if reached |

Index: `approvals_pending ON approvals(session_id, state) WHERE state='pending'`.

## shares

Files an agent has put behind a link with `share_file` (v56), served by the
web server at `/share/<token>/<name>`. The row is the share: nothing is
copied, and an expired row simply stops matching. Expired rows are deleted
when the next share is created.

| Column | Type | Meaning |
|--------|------|---------|
| `token` | TEXT PRIMARY KEY | 32 hex characters from `util_random`. The whole capability: holding it is what permits reading the file. |
| `agent_name` | TEXT | The agent that shared it (cascades with the agent). |
| `path` | TEXT | The file as resolved when it was shared. It is served only while the file opened at this path still resolves to this path. |
| `name` | TEXT | Basename reduced to `[A-Za-z0-9._-]`, for the URL and `Content-Disposition`. |
| `created_at` | INTEGER | Unix time of the share. |
| `expires_at` | INTEGER | Unix time the link stops working. |

## approval_decisions

A queue of decisions made off the event loop (v54). Only the event loop
resolves an approval, because resolving re-runs a tool and advances a session.
The web dashboard runs on other threads. It inserts a row here and wakes the
loop; the loop removes the row and calls `resolve_approval()`. The first
decision for an approval wins. A row normally lives for milliseconds; the
periodic tick drains any row whose wake was lost.

| Column | Type | Meaning |
|--------|------|---------|
| `approval_id` | INTEGER PRIMARY KEY | The approval the decision is for. FK → `approvals(id)` (v59): a decision for an approval that does not exist is refused. |
| `decision` | TEXT | `deny`, `once` or `always`. |
| `decided_via` | TEXT | Copied to `approvals.decided_via` (for example `dashboard`). |
| `created_at` | INTEGER | Unix time of the decision. |

`park_reason` has exactly two values (v47 rekey, config-doc M3). `sensitive_target` is the sensitivity-axis overlay — per-call authority, ALWAYS coerced to ONCE at resolve, never transferable as a ticket; everything else parks as `approval_required`. What a row is *about* is `tool_name` alone (card dispatch, dedup, and apply all key on it), so the old dual-purpose `action` column — sometimes a tool name, sometimes a document verb, sometimes `sensitive` — is gone. The former `secret_bind` action was deleted with the bind park (D17) — an unbound secret denies inline and the agent requests the binding via `request_config` `secret_bindings`.

---

## sensitive_targets

Sensitivity axis (specs/trust.md): operator-owned labels on targets, global — not per-agent. The one place a label subtracts from authority: labeled hosts ride every shell and js call as proxy deny-before-allow rules, and dispatch parks any call whose args reference one. Written only by `cclaw sensitive add|rm` (no agent tool).

| Column | Type | Notes |
|--------|------|-------|
| `kind` | TEXT NOT NULL DEFAULT 'host' | only `host` in v1 |
| `value` | TEXT NOT NULL | exact host or `.suffix` rule; bare domains cover subdomains |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |
| PRIMARY KEY | (kind, value) | |

---

## secret_hosts

Fail-closed credential rule (specs/trust.md): a secret may only be submitted to hosts it is bound to. Zero bindings → the gate denies inline with the missing pair named; shell/js calls carrying a secret get their egress narrowed to the union of bound hosts, unconditionally. Rows come from `cclaw secret-bind` or an approved `request_config` `secret_bindings` document.

| Column | Type | Notes |
|--------|------|-------|
| `secret_name` | TEXT NOT NULL | matches the `{{SECRET:name}}` placeholder. Deliberately **not** an FK to `secrets`: a `CCLAW_SECRET_*` environment secret is bound here but has no `secrets` row |
| `host` | TEXT NOT NULL | exact host or `.suffix` rule |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |
| PRIMARY KEY | (secret_name, host) | |

---

## secrets

Secret store (specs/security.md): the single home for every encrypted value, all AEAD-sealed with the master key. `scope` splits two audiences: `agent` secrets feed the per-call snapshot (`secrets_snapshot()` merges them with the env-collected `CCLAW_SECRET_*` base — env wins on a name collision) and are usable via `{{SECRET:name}}` by an agent holding a `secret` grant for the name (v60); `system` secrets are provider API keys consumed by the daemon (`db_secret_get_system()`) and are **excluded from the snapshot**, so no agent can interpolate them and none can be granted one. Two gates apply to every use: the grant (who) and `secret_hosts` (where — zero rows for the name denies, fail-closed).

| Column | Type | Notes |
|--------|------|-------|
| `name` | TEXT PRIMARY KEY | `^[A-Z][A-Z0-9_]*$` — becomes `CCLAW_SECRET_<name>` / `{{SECRET:name}}` |
| `value` | TEXT NOT NULL | `enc:<hex(...)>`, never plaintext |
| `source` | TEXT NOT NULL DEFAULT 'operator' | `operator` (`cclaw secret set`) \| `generated` (`secret_create` tool) \| `captured` (`save_secret` on a tool call) |
| `scope` | TEXT NOT NULL DEFAULT 'agent' | `agent` (interpolatable/injectable) \| `system` (provider keys, daemon-only) |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

Written by: `cclaw secret set\|rm\|list\|grant\|revoke` (operator verb), the `secret_create` tool, `admin_set_key` (scope `system`), and `secret_capture_apply` (`save_secret` on web_fetch/shell_exec/js_eval). The last two go through `db_secret_capture`, which stores the secret and grants it to that agent (source `capture`) in one transaction; `db_secret_rm` deletes the row, its `secret_hosts` rows and its `secret` grants in one transaction. The DLP scanner never writes here — it redacts. (A `status` column marked scanner-quarantined rows until schema v20 removed quarantine.)

---

## agent_extensions

Links agents to their enabled extensions.

| Column | Type | Notes |
|--------|------|-------|
| `agent_name` | TEXT | FK → `agents(name)` ON UPDATE CASCADE; NULL = global |
| `extension_name` | TEXT NOT NULL | FK → `extensions(name)` ON UPDATE CASCADE (v59) |
| `config` | TEXT | JSON config blob |
| `enabled` | INTEGER NOT NULL DEFAULT 1 | |
| PRIMARY KEY | (agent_name, extension_name) | |

---

## channels

Channel processes managed by the daemon. The extension system writes these rows.

| Column | Type | Notes |
|--------|------|-------|
| `name` | TEXT PRIMARY KEY | |
| `extension_name` | TEXT | FK → `extensions(name)` ON UPDATE CASCADE (v59); the code fronting the channel. NULL = none (was `''` before v59) — such a channel never launches |
| `prev_extension_name` | TEXT | FK → `extensions(name)` ON UPDATE CASCADE (v59); revert target recorded by channel swap |
| `type` | TEXT NOT NULL DEFAULT '' | the manifest's `$.channel.type` (`telegram`, `discord`, `whatsapp`, …); display only (the dashboard's channel list). `binary_path` (written, never read) was dropped in v59 — the runner resolves the handler through `extensions.path` |
| `status` | TEXT NOT NULL DEFAULT 'draft' | see lifecycle below |
| `pid` | INTEGER | daemon tracks the running process |
| `default_agent` | TEXT | FK → `agents(name)`. Open-door policy: non-NULL = unrouted chats are accepted and get a new session for this agent; NULL = fail-closed |
| `default_tool_filter` | TEXT | JSON array of tool names; NULL = unrestricted. Copied once onto every chat the gate records (`channel_chats.tool_filter` of a new row) — open door **and** admin fallback alike; an edit changes future chats only. Set by `route add <ch> '*' <agent> --tools ...` |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

Status lifecycle: `draft` → `validated` → `active`.
- `extension_install()` inserts as `draft`.
- `cclaw --channel <name> --check` validates config → `validated`.
- `--activate` promotes → `active`; `channel_launch_all()` only execs channels in this state.
- A crash-looping channel stays `active`: `channel_reap()` retries it on a growing backoff (see [channels.md](channels.md#crash-supervision)). `broken` is only found in rows an older build left behind; `--check` lifts it.

---

## channel_state

Channel-private persistent key-value store (offsets, cursors, tokens).

| Column | Type | Notes |
|--------|------|-------|
| `channel_name` | TEXT NOT NULL | FK → `channels(name)` ON UPDATE CASCADE (v59) |
| `key` | TEXT NOT NULL | |
| `value` | TEXT | |
| PRIMARY KEY | (channel_name, key) | |

---

## channel_chats

One row per chat (platform conversation id) a channel has seen or an operator
or agent has named (v61, replacing `channel_routes`; plan v0.8 decisions
1–10). The pin (`session_id`) says which session the chat's messages go to;
the session names its agent. The chat's three settings are read live — by
the session the chat pins (`tool_filter`) and by every session that belongs
to the chat (`delivery_mode`, `guidance`); nothing is copied onto a session.
A new row starts from defaults copied once. Rows are never deleted. Logged in
`change_log` (settings: the key, `session_id`, `delivery_mode`,
`tool_filter`, `guidance`; the rest are measurements).

| Column | Type | Notes |
|--------|------|-------|
| `channel_name` | TEXT NOT NULL | FK → `channels(name)` ON UPDATE CASCADE; a chat cannot move between channels |
| `chat_id` | TEXT NOT NULL | Telegram chat id, Discord channel/thread/DM id, … |
| `session_id` | INTEGER | FK → `sessions(id)`: the pin. NULL = not routed (never assigned, or removed by `route rm` — the one way a routed chat becomes unrouted) — the gate drops the chat's messages unless the channel's door is open. Unique where not NULL (`idx_channel_chats_session`): one chat per session. A pinned session names its chat |
| `delivery_mode` | TEXT NOT NULL DEFAULT 'auto' | CHECK `auto` (= `turn`) \| `iteration` \| `digest` \| `turn` \| `quiescent` \| `explicit`. The only value consulted for the delivery of the chat's sessions, read at every boundary ([delivery.md](delivery.md)); also shapes the system prompt's reply line. A new row: from the envelope's `chat_type` (group `explicit`, dm `auto`); a chat named before it spoke, by its id (begins with `-`: `explicit`). Never overwritten by a later message |
| `tool_filter` | TEXT | JSON array of tool names; NULL = unrestricted. The pinned session's tools are grants ∩ `sessions.tool_filter` ∩ this, read live. A chat the gate records starts from `channels.default_tool_filter` |
| `guidance` | TEXT | Appended to the system prompt of the chat's sessions every request (`route add --prompt`), after the location and reply lines; NULL = nothing |
| `title` | TEXT | display name as observed by the channel runner (`#general @ My Server`); shown in the system prompt. NULL until a runner reports one |
| `is_admin` | INTEGER | 1 = the chat's only reader is an operator (a DM whose other party is one of the channel's `admin_ids`); 0 = any other chat. Set from each incoming message; NULL until one arrives. Decides whether a failed turn sends the chat the generic line ([error-handling.md](error-handling.md#a-turn-that-ends-in-an-error)) |
| `error_notice_at` | INTEGER | Unix time the chat last got the generic "can't respond" line; the once-per-`chat_error_notice_hours` limit |
| `kind` | TEXT | CHECK `dm` \| `group` — the envelope's `chat_type`; NULL = not reported (every chat migrated in v61 until it speaks again) |
| `first_seen_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | v61 migration: the oldest session or cron job naming the chat |
| `last_message_at` | INTEGER | the last inbound message, routed or dropped |
| `notified_at` | INTEGER | when the operator was told this unrouted chat spoke (once per chat; survives restarts; `route rm` clears it) |
| PRIMARY KEY | (channel_name, chat_id) | |

The v61 migration moved the cursor of every channel edge that never posted —
frozen `explicit` under v60, or whose chat is `explicit` now — to its
session's `leaf_id` (what an explicit boundary stamps today), so no later
flip to `auto` posts an old answer; other edges kept their cursor (behind =
owed under v60 too). It made one row per pair any route, session or cron job named;
where the values came from, and how it resolved what the new constraints
refuse (two chats pinning one session, stamps on a channel with no row), is
in the comment on `patch_v61_chats()` (`src/db.c`).

---

## sessions

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `name` | TEXT | human label |
| `agent_name` | TEXT | FK → `agents(name)` ON UPDATE CASCADE; scopes session to agent |
| `channel_name` | TEXT | with `chat_id`: the chat this session belongs to — FK (composite) → `channel_chats` ON UPDATE CASCADE (v61); both NULL = none. Older sessions left behind by `/new` still name their chat; `channel_chats.session_id` says which one is pinned now |
| `chat_id` | TEXT | see `channel_name` |
| `parent_session_id` | INTEGER DEFAULT -1 | sub-agent parent (-1 = top-level) |
| `parent_tool_call_id` | TEXT | the tool_call that spawned this sub-session |
| `depth` | INTEGER NOT NULL DEFAULT 0 | sub-agent nesting depth |
| `tool_filter` | TEXT | JSON array of tool names (see below) |
| `state` | TEXT NOT NULL DEFAULT 'idle' | see state machine below |
| `owner_instance` | TEXT | FK to `processes.instance_id`; NULL ⟺ idle |
| `settle_event` | TEXT | v68. While `state` = `settling`: the event the hold began with (`Stop`, `SubagentStop`, `ContextThreshold`, `CacheExpiring`), set in the same write as the state (`session_set_settling`) and cleared by every other transition and by recovery. Whoever ends the hold — the reap or the sweep — reads it: no C check follows `CacheExpiring` ([hooks.md](hooks.md#the-hold)). NULL in every other state |
| `turn_iteration` | INTEGER NOT NULL DEFAULT 0 | iteration within current turn |
| `turn_context` | TEXT | The user context block (`<RELEVANT_CONTEXT>` text), materialized once at turn start (`llm_proc.c`) and reused verbatim by every tool-loop iteration so the request prefix stays byte-stable for prompt caching; always present (it carries `<current_time>` even when nothing else is live) |
| `turn_context_tokens` | INTEGER | v67. The provider's count for the context block the newest measured request carried; the next turn's first report gives it back before sharing the growth in `usage_in` (`usage_attribute`). The block's estimate when that report could not be compared; NULL = no report yet |
| `leaf_id` | INTEGER DEFAULT -1 | current branch tip entry |
| `cache_armed_at` | INTEGER | v68. The `CacheExpiring` arm: when the session's last answered request started, set when the governing entry has a `cache_lifetime`. Cleared when the event fires or the cache has lapsed, so it fires once per idle period (`src/cache_expiring.c`). NULL = not armed |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |
| `updated_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

Indexes: `idx_sessions_owner ON sessions(owner_instance) WHERE owner_instance IS NOT NULL`;
`idx_sessions_cache_armed ON sessions(cache_armed_at) WHERE cache_armed_at IS NOT NULL` (v68).
`compaction_fail_count` was dropped in v68.
The chat columns that lived here until v61 (`chat_title`, `chat_is_admin`,
`chat_error_notice_at`) are `channel_chats.title`, `is_admin`,
`error_notice_at`.

### delivery_edges

The delivery contract lives in [delivery.md](delivery.md); this is the shape.
One row per outbound edge of a session: the standing parent edge every child
gets at creation (v42 replaced `sessions.parent_notified_at` with this
table), the channel edge a session gets in the same write that binds it to a
chat, cursor at its leaf (v69; no policy of its own — its chat's `delivery_mode` is read live), and the one-shot `tool_call` edge a blocking
launch adds (deleted when it fires). `target_kind='session'` + one-shot reply
edges are reserved for milestone 2's `session_send`.

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `session_id` | INTEGER NOT NULL | FK → `sessions(id)`; the session whose output this edge carries |
| `target_kind` | TEXT NOT NULL | `'parent'` \| `'channel'` \| `'session'` \| `'tool_call'` |
| `target_ref` | TEXT NOT NULL | parent: parent session id; channel: channel name (chat_id lives on the session); tool_call: call_id |
| `policy` | TEXT | `iteration` \| `digest` \| `turn` \| `quiescent` \| `explicit`, frozen; NULL exactly on channel edges (CHECK, v61) — read through `EDGE_POLICY` (db.h) |
| `cursor` | INTEGER NOT NULL DEFAULT 0 | last entry id this edge delivered; stamped inside the delivery transaction. Cursor behind an idle session's assistant leaf = delivery owed — `advance_sweep_undelivered()`'s re-derivation predicate (the convergence sweep, `db_periodic`) |
| `one_shot` | INTEGER NOT NULL DEFAULT 0 | blocking reply edge: fires once, then the row is deleted |

Unique: `(session_id, target_kind, target_ref)`.

The v69 patch gave every chat-bound session that had no channel edge one at
its current `leaf_id` (0 for none): the sweep can find a reply cut off before
delivery from then on, and the upgrade owes nothing already said.

### sessions.state values

| State | Meaning |
|-------|---------|
| `idle` | No work in progress; `owner_instance` is NULL |
| `llm_running` | LLM request in flight on a worker thread |
| `tool_running` | Tool execution in progress |
| `rate_limited` | Parked waiting for rate-limit window |
| `awaiting_approval` | Parked for human approval of a tool call |
| `settling` | An awaited hook script is running ([hooks.md](hooks.md#the-hold)): at a turn's end, between two iterations, or on an idle session whose cache is expiring. Entered from `idle` or `tool_running`, left to `idle` or `tool_running` only; the inbox queues and is not consumed; counts as holding a resource for `session_max_concurrent`. A dead owner's `settling` is recovered to `idle`; a turn held between iterations is then resumed by the daemon's inbox sweep, which reads the leaf through notes and summaries ([hooks.md](hooks.md#a-hold-cut-short)) |

Transitions are guarded by a CAS in `session_set_state()` (`src/db.c`): idle clears `owner_instance`, any busy state stamps it. `turn_iteration` resets to 0 on transition to idle.

An idle session whose leaf entry is unanswered — role 1 (dispatch refused after the inbox drain) or role 3 (a mid-turn dispatch bounce, a zombie call closed at turn start, or recovery of a session a dead process held mid-turn) — is not resting: `advance_session` resumes it, and the daemon tick's `session_sweep_inbox` wakes it with the same predicate. The leaf is read through notes and summaries (roles 0 and 4: a hook may leave one at the end of a running turn, the streak guard after an answer) to the entry before them (`CCLAW_SQL_LEAF_READ_THROUGH`, `src/advance.h`); the `llm_running` lost-submit check reads it the same way. A role-3 resume continues the *same* turn, so it re-derives `turn_iteration` from the leaf turn's distinct `iteration_id`s (the idle transition above zeroed it) and keeps the frozen `turn_context`. A role-2 entry, read through, is a turn at rest.

### The autonomous-turn streak (cross-turn runaway guard)

`max_iterations` bounds a turn; nothing bounded turns *generating* turns with no human in the causal chain (cron chatter, sub-agent respawn ping-pong). The turn-open branch of `advance_session` (`idle`) checks the streak **before** the drain. **Metric**: `COUNT(DISTINCT turn_id)` over the session's entries newer than the last *human* turn, where a human turn is the most recent one holding a role-1 entry whose `data.source` is not in `('cron','cron_error','agent_result','spawn','system')` — or holds no stamp at all (only the drain stamps, so unstamped means a human-adjacent writer). No human turn ever ⇒ every turn counts. The classification is autonomous-side because the human side is open-ended: `cli`, any channel name, `approval`. There is no counter column; the SQL fragments live in `src/advance.h` and are shared with `--doctor`. **Cap**: config `max_autonomous_turn_streak` (default 50, 0 = off). At or past it, a turn open whose *entire* pending inbox batch is autonomous consumes nothing and returns `ADVANCE_NOOP` — the inbox rows stay durable and the session simply goes quiet (the daemon sweep keeps waking it; each wake NOOPs at the gate for two indexed counts). An unanswered-leaf resume is never gated: the guard only fires when there is queued work. **Recovery**: a human row anywhere in the pending batch lets the turn through, and opening it resets the streak by construction. **Notices**: at ~80% of the cap an allowed turn gets one role-0 entry stamped `{"source":"streak_guard","note":"warn"}` appended to its opening batch; the first refusal of a trip writes one stamped `…"note":"tripped"` (role 0, so it neither counts toward the streak nor makes a leaf to resume: it is read through to the answered turn before it) plus a `channel_outbox` notice to the operator (the channel's admins; see [error-handling.md](error-handling.md#operator-notices)) and a syslog WARN. Once-per-trip is derived from that marker being newer than the last human turn — a human turn later starts a fresh trip.

The same choke point carries a second, unrelated gate: the
`session_max_concurrent` drain-side concurrency gate (defer-not-refuse, human
batches pass, resource-holder counting) — contract in
[specs/scheduling.md](scheduling.md). It shares the streak guard's shape
(before-the-drain check, NOOP with durable inbox rows, same human/autonomous
classification) but bounds box load, not loop length, and defers until a
resource frees rather than until a human replies.

**The classification fails open — know this before adding a writer.** Anything unknown or unstamped reads as *human*: it bypasses the guard and resets the streak. That is correct for every writer today (only the drain stamps; the CLI's direct append is genuinely human), but it means a future autonomous writer that forgets to stamp, invents a source name outside the fixed list, or arrives dressed as a channel (a self-augmented JS channel wrapping a machine feed — an RSS poller, a webhook) is silently exempt from the very loop-bound this guard exists to enforce. There is no enum enforcement on `source` — a typo'd `'agent-result'` is a human. If you add an autonomous entry point, add its source to `CCLAW_AUTONOMOUS_SOURCES` (`src/advance.h`) in the same commit; if the list starts churning or machine-fed channels become real, the fix is to make autonomy an explicit bit the writer declares at inbox-insert time, not to keep growing the name list.

### sessions.tool_filter

A JSON array of tool names frozen at sub-agent spawn time (`launch_agent`). Acts as a **positive scope** intersected with grants per turn — it can never widen authority, only shrink it. NULL means unrestricted (uses grants alone). A chat's filter is not copied here (v61): the session a chat pins is also intersected with `channel_chats.tool_filter`, read live. Sessions created before v61 from a filtered route still carry their copy; intersecting it again changes nothing.

---

## entries

Split-column format — no JSON parsing on LLM request hot path. `llm_payload.c` builds the wire JSON directly from columns via `json_object()`.

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `session_id` | INTEGER NOT NULL | |
| `parent_id` | INTEGER NOT NULL DEFAULT -1 | tree structure (Pi model); -1 = root |
| `original_parent_id` | INTEGER | the parent the entry was written under, set when a [turn edit](turn-edit.md) re-points it. A turn edit keeps the first value (`COALESCE`); rows it takes off the branch are not touched and still point at their own parent, so the turn as written can be walked back |
| `turn_id` | INTEGER | the turn this entry belongs to — minted when a user entry follows a non-user one, inherited from the parent thereafter (trigger `entries_turn_ai`). The request cut snaps to it so it never splits a turn; whole-turn edits name it. An edit's summary carries the first turn it replaced (between turns) or the running turn's id (a summary of its iterations); a note, the turn of the place it goes |
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
| `cached_tokens` | INTEGER | cache-read subset of `usage_in`; NULL when the provider doesn't report it, 0 when it reports none (rows ingested before v68 stored a reported 0 as NULL). Discounted to 0.25 weight by `rate_limit_check`; read by the explorer's cache view |
| `cost_nano` | INTEGER | nanodollars |
| `token_estimate` | INTEGER | `content_bytes` at 2.5 bytes per token + 4 (`TOKEN_ESTIMATE` in `types.h`; a reasoning entry the estimate of an empty entry, its cost being its replay); v68 recomputed existing rows from `content_bytes` |
| `tokens` | INTEGER | v67. The provider's count for this entry, written once when a response reports it (`usage_attribute`, `db_response.c`): its share of `usage_out` for what the model wrote, its share of the growth in `usage_in` for what was appended. NULL = not measured, read as `token_estimate`. A reasoning entry holds what its replay costs — 0 once it is no longer sent or counted. See [memory.md](memory.md#context-window-management) |
| `request_cut` | INTEGER | v68. On a reported `assistant_message`: 1 when the request that produced it had been cut (its oldest turns left out to fit the budget, `context_plan`), stamped by `llm_req`; its `usage_in` then does not measure the head and the head estimate stands. NULL = sent whole |
| `content_bytes` | INTEGER | byte length of content + tool_calls |
| `tool_call_count` | INTEGER NOT NULL DEFAULT 0 | denormalized for plan pass |
| `data` | TEXT | JSON side-channel, merged not overwritten (`json_patch`), nullable. `$.source` / `$.source_ref` — provenance stamped by the inbox drain (the runaway-turn guard and the cron auto-pause streak read it), and on a note or summary an edit wrote, who wrote it (`hook:<extension>`, `fallback` for C's trim, `tool:session_condense`); a stand-in keeps its original's; `$.edited_by` on every entry an edit wrote — the edit's label; `$.replaces` on an edit's stand-in — the first original it stands for, which readers of `$.source` that count events (the cron auto-pause streak) skip ([turn-edit.md](turn-edit.md#edited-entries)); `$.job` on a `cron_result` — the job that produced it; `$.turn_edits` on the entry that hosts an edit's record — see [below](#entriesdata-turn_edits) |
| `network_hosts` | TEXT | JSON array of hosts the tool run contacted (proxy-observed) |
| `fence_id` | TEXT | Random id minted when `network_hosts` is set; see below |
| `reasoning_meta` | TEXT | `type='reasoning'` rows only — replay tag+blob, see below |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

### entries.content by type

| type | content holds |
|------|---------------|
| `system` | System prompt text, or a mid-conversation note (`role=0`, `iteration_id=0`). Sent as user text `[system] <text>`, except a note whose `data.source` is `hook:<ext>` — left by a [hook](turn-edit.md) — which is sent as `[note from extension <ext>] <text>` |
| `user_message` | User's message text |
| `assistant_message` | Assistant's text response (may be NULL if tool_calls only) |
| `tool_call` | **Validated JSON arguments** (normalized via `json()` at ingest; single source of truth for tool call args) |
| `tool_result` | Tool execution output (text) |
| `reasoning` | The reasoning text the provider returned — a summary on Anthropic and Gemini, whatever the upstream sends on an OpenAI-compatible provider — kept when the routing entry's `reasoning_display` is `summary`; empty under `none`. Replay reads `reasoning_meta`, never this |
| `compaction` | A summary an edit wrote in place of a run of turns or iterations (a hook's `summarise`, `session_condense`); `role=4` |
| `cron_result` | A scheduled script's output, posted with no LLM call behind it. `role=2` so ordinary delivery finds it and `iteration_id=0` because it spent no iteration; `data.job` names the job and `is_error` records how the run went. The payload serializer labels it (`[scheduled script <job> output]`) rather than passing it through bare — the model must not read a machine's stdout back as its own words |

### entries.data `$.turn_edits`

One element per applied [edit](turn-edit.md#the-record), appended in the
edit's transaction on one entry: the session's leaf after a script's edit
(at a turn's end, its answer), the note C's trim wrote. Other keys on the
entry are kept. Never sent to the model.

```json
{"turn_edits": [{"by": "hook:compaction", "at": 1763300000,
                 "left_branch": [412, 415, 416],
                 "replaced": [{"original": 204, "replacement": 431}],
                 "notes": [432], "summaries": [433],
                 "tokens": {"before": 91200, "after": 43100},
                 "thinking_cut": 431,
                 "record": {"…": "the script's own, when it sent one"}}]}
```

- `left_branch` — every original taken off the branch, in chain order (replaced and rewritten originals included). The rows stay in the table.
- `replaced` — per stand-in, the original and the new row in its place; `notes`, `summaries` — the entries the edit inserted. All in edit order.
- `tokens` — the branch's size before and after: each entry at `tokens`, or `token_estimate` where unmeasured (C's trim: the session's size, head included).
- `thinking_cut` — the first entry of the new branch that is new or re-pointed; reasoning produced before the edit at or after it is not replayed on Anthropic. Null when only the tail went.
- C's trim adds `dropped` `{turns, iterations}` and `limits` `{window, threshold, target}`.

### entries.reasoning_meta

`{provider, model, format, blob}` on `type='reasoning'` entries. `provider`
(`openai`/`gemini`/`anthropic`) and `model` name who produced the reasoning; the payload
builder replays it only when the requesting model matches, so a model switch
strips it (reasoning is model-bound). `format` says what `blob` is and how it
goes back on the wire:

| format | blob | replayed as |
|--------|------|-------------|
| `reasoning_details` | OpenRouter's `reasoning_details` array, verbatim | `reasoning_details` on the assistant message — order and every field preserved; this is how Gemini's `thoughtSignature` survives the OpenAI-compat envelope |
| `reasoning_content` | the bare reasoning string | `reasoning_content` on the assistant message (DeepSeek's rule after a tool-call turn) |
| `gemini_parts` | `[{fn, sig}, …]` | `thoughtSignature` on the matching replayed `functionCall` part |
| `anthropic_content` | the response's whole `content` array, verbatim | the assistant message's `content`, in place of the one rebuilt from entries — within the producing turn only (see [providers.md](providers.md#anthropic-messages)) |

The blob — not `entries.content` — is what replay reads: the routing entry's
`reasoning_display` governs whether the *display* text is kept, while the
wire requirement holds either way. No blob (pre-v48 rows, or a provider that sent none) → nothing is
emitted; a reasoning field is never invented.

### entries.network_hosts

NULL or a JSON array of hostnames the credential-proxy observed during a shell tool execution. Stored for auditing and grant enforcement replay.

### entries.fence_id

A tool result with `network_hosts` set is sent to the model wrapped as
untrusted content (`SQL_WRAP_TOOL_CONTENT` in `llm_payload.c`):

```
<UNTRUSTED_EXTERNAL_CONTENT id="9f2c4e1ab07d53c8">
Contacted hosts: ["example.com"]
---
…the result…
</UNTRUSTED_EXTERNAL_CONTENT id="9f2c4e1ab07d53c8">
```

`fence_id` is that id: 16 hex digits from `randomblob(8)`, written by
`db_entry_set_network_hosts` in the same UPDATE that tags the entry — after the
content exists, so a fetched page cannot contain it and cannot close the block
early. Content is not scanned for tag lookalikes; the id is the defence. It is
stored rather than generated per request so the request prefix stays
byte-stable for prompt caching.

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
idx_entries_iteration_type (session_id, iteration_id, type, part_index) -- covering: the payload transcript's reasoning lookup
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
| `status` | TEXT NOT NULL DEFAULT 'pending' | pending / running / background (a detached job; `resolved_by` holds the dispatching instance until completion) / done |
| `resolved_by` | TEXT | |
| `resolved_at` | INTEGER | |

Indexes: `idx_tool_calls_entry(entry_id)`, `idx_tool_calls_session(session_id, status)`.

---

## tools

Tool registry. Built-in C tools have NULL `extension_name`/`path`; JS tools point to a handler file in the shared extension store. A shipped (ownerless) extension's install takes over a C tool's row of the same name — how `web_fetch` became the `web-fetch` extension's tool on an existing DB with no patch; an agent's promote never does.

| Column | Type | Notes |
|--------|------|-------|
| `name` | TEXT PRIMARY KEY | |
| `extension_name` | TEXT | FK → `extensions(name)` ON UPDATE CASCADE (v59); NULL for built-in C tools |
| `description` | TEXT | |
| `parameters_json` | TEXT | JSON Schema for arguments |
| `path` | TEXT | handler file in extension store |
| `agent_name` | TEXT | FK → `agents(name)` ON UPDATE CASCADE; owner scope; NULL = global |
| `enabled` | INTEGER NOT NULL DEFAULT 1 | |
| `policy` | TEXT | JSON restrict-only argument policy |
| `egress_hosts` | TEXT | comma-separated hosts declared by the manifest; non-NULL **replaces** the agent's host grants for calls of this tool. Only a promoted extension tool can carry one — see [extensions.md](extensions.md#declared-reach-hosts) |
| `droppable` | INTEGER NOT NULL DEFAULT 0 | v63. 1 = a [hook](turn-edit.md#protected-calls) may drop a call of this tool on its own; 0 = protected (its result's text can still be replaced). Built-in tools: the recipe's `droppable` flag, written by `tools_sync_to_db`. Extension tools: `$.tools[].droppable` in the manifest, 0 when unstated |

---

## hooks

Extension-provided hooks. Same provenance model as tools — the DB holds config + a path, never code. Events: `Stop`, `SubagentStop`, `ContextThreshold`, `CacheExpiring` ([hooks.md](hooks.md)); the manifest validator refuses any other name. Rows are written by promote, one per `$.hooks[]` entry, and read at each event for the session's agent (never cached).

| Column | Type | Notes |
|--------|------|-------|
| `extension_name` | TEXT NOT NULL | FK → `extensions(name)` ON UPDATE CASCADE (v59) |
| `event` | TEXT NOT NULL | event name |
| `path` | TEXT NOT NULL | handler file in extension store |
| `enabled` | INTEGER NOT NULL DEFAULT 1 | the operator's switch; never set by the runtime |
| `async` | INTEGER NOT NULL DEFAULT 0 | v68 (replaces `wait`, its inverse). 0 = the session is held in `settling` until the script ends and its reply is applied; 1 = started, not awaited, output discarded. From `$.hooks[].async` |
| `fail_count` | INTEGER NOT NULL DEFAULT 0 | v64. Consecutive failed runs of this hook, any session; a success zeroes it, re-promote resets it |
| `input` | TEXT | v66. JSON: what the script is sent besides the newest turn — `$.hooks[].input` as declared, `{history, history_results, max_bytes}`, validated at promote. NULL = nothing declared; each option a row does not name takes its default when the hook runs (`hook_next`): the turn alone, results with their text, at most 1 MB. See [turn-edit.md](turn-edit.md#input) |
| `pos` | INTEGER NOT NULL DEFAULT 0 | v68. The entry's index in the manifest's `hooks[]`. Hooks for one event run in `(extension_name, pos, path)` order |
| `timeout` | INTEGER | v68. Seconds the script may run; CHECK 1–3600 (`HOOK_TIMEOUT_MAX`; promote admits the same range). NULL = config `hook_timeout` |
| `disabled` | INTEGER NOT NULL DEFAULT 0 | v68. 1 = turned off after `HOOK_FAIL_LIMIT` (3) failures in a row; the default agent was told once. Cleared by re-promote, by a new version of a bundle shipped in the binary, or by hand (`UPDATE hooks SET disabled=0, fail_count=0 …`) |
| PRIMARY KEY | (extension_name, event, path) | |

The v68 patch copies `wait` into `async` (inverted) and drops `wait`;
existing rows get `pos` 0, no timeout, enabled.

---

## memory_blocks

Named memory blocks. A block is a pure **container**: it carries a label, char limit, placement and optional read-only flag, but no text of its own — its content is the `memory_notes` rows that name it. Which agents see it is `agent_memory_blocks`. See [memory.md](memory.md).

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `created_by` | TEXT | The agent that created the block (v59; was `agent_name`). History, not a permission: it grants nothing. FK → `agents(name)` ON UPDATE CASCADE; NULL = made by the operator. Who sees or writes it is `agent_memory_blocks` alone |
| `label` | TEXT NOT NULL | how an agent addresses the block |
| `description` | TEXT | tells agent what block is for |
| `char_limit` | INTEGER NOT NULL DEFAULT 5000 | budget for the notes' total length |
| `read_only` | INTEGER NOT NULL DEFAULT 0 | 1 = no agent may change the block |
| `placement` | TEXT NOT NULL DEFAULT 'system' | `system` = rendered into the system prompt once per session (`agent_config.c`); `context` = rendered into the user context block (`<RELEVANT_CONTEXT>`) at each turn start. New blocks default to `context` via `memory_block_create` (the API default every insert path goes through — the column DEFAULT is inert); only the Assistant's seeded AGENT/USER identity blocks ask for `system` |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |
| `updated_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |
| UNIQUE | (created_by, label) | |

---

## agent_memory_blocks

Which agents have which blocks (v57). A block with more than one row here is shared. No agent may hold two attachments whose blocks share a label — enforced by `memory_block_attach`, since a label is how the agent names the block.

| Column | Type | Notes |
|--------|------|-------|
| `agent_name` | TEXT NOT NULL | FK → `agents(name)` ON UPDATE CASCADE ON DELETE CASCADE |
| `block_id` | INTEGER NOT NULL | FK → `memory_blocks(id)` ON DELETE CASCADE |
| `read_only` | INTEGER NOT NULL DEFAULT 0 | per attachment; the owner is attached with 0, anyone else with 1 unless the operator chooses otherwise |
| PRIMARY KEY | (agent_name, block_id) | |

---

## memory_notes

The notes inside a block (v57; replaces `memory_entries`). A note is addressed by `id`, which is never reused or renumbered. Display order is `ORDER BY id`.

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | the handle shown to the agent as `[id]` |
| `block_id` | INTEGER NOT NULL | FK → `memory_blocks(id)` ON DELETE CASCADE |
| `text` | TEXT NOT NULL | |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `agent_name` | TEXT NOT NULL | FK → `agents(name)` ON UPDATE CASCADE |
| `block_label` | TEXT NOT NULL | |
| `pos` | INTEGER NOT NULL | ordering position |
| `text` | TEXT NOT NULL | |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |
| `updated_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | last time the text changed; shown to the agent as a date beside the id |

Index: `idx_memory_notes_block ON memory_notes(block_id, id)`.

---

## inbox

Durable inbound message queue. Rows survive crashes. `inbox_consume_into_entries` consumes them atomically inside `BEGIN IMMEDIATE`.

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `session_id` | INTEGER NOT NULL | |
| `source` | TEXT NOT NULL DEFAULT 'cli' | channel origin |
| `source_ref` | TEXT | provenance whose semantics belong to `source`: `'cron'`/`'cron_error'` → job name, `'agent_result'` → child session id. NULL for every other writer |
| `payload` | TEXT NOT NULL | |
| `consumed` | INTEGER NOT NULL DEFAULT 0 | |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

Index: `idx_inbox_pending ON inbox(session_id, consumed) WHERE consumed = 0`.

`inbox_consume_into_entries` stamps a non-NULL `source_ref` onto the entry
content as a bracket tag — `[cron: <name>] `, `[sub-agent session <id>] `, else
`[<source> <ref>] ` — so the reference stays model-visible without a join the
referent may not survive (a one-shot job deletes itself at fire time). A NULL
`source_ref` drains verbatim.

The same drain also writes the provenance *structurally* into
`entries.data` — `{"source": …, "source_ref": …}`, the `source_ref` key
omitted (never a JSON null) when there is none. Prose can be compacted away;
this is what stays queryable, and it is what the cron auto-pause streak and
the autonomous-turn streak (see sessions) read to tell a job's fires — and a
sub-agent's result, and a system notice — apart from a human's message.

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
| `status` | TEXT NOT NULL DEFAULT 'pending' | pending / sending / delivered / `failed:<reason>` |
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
or the agent-level pulse — is a durable row here, fired by
`run_due_jobs()` off the daemon's existing 30s `db_periodic` tick. There is
no separate heartbeat thread and no heartbeat *kind*: the pulse is an
ordinary bare-wake job named `heartbeat` (no payload), so the same fire path
serves it.

A job's schedule is exactly one of `cron_expr` (recurring, 5-field),
`run_at` (one-shot unix timestamp), or `interval_s` (fixed period in
seconds) — the unused fields are `''` / NULL / NULL respectively
(`cron_list()` reads the NULLs back as 0). Payload is `task` and/or
`script`, orthogonal to schedule; both empty is a bare wake.

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `agent_name` | TEXT | FK → `agents(name)` ON UPDATE CASCADE |
| `name` | TEXT NOT NULL | |
| `cron_expr` | TEXT NOT NULL | Fields are matched in the **daemon's local timezone** (`localtime_r`/`mktime`), so a "9am" job follows DST; the `<current_time>` line in the user context block states that zone so the model can write the expression. `''` sentinel when `run_at` or `interval_s` is used instead (kept NOT NULL — SQLite can't relax it without a table rebuild) |
| `run_at` | INTEGER | one-shot fire time; row is deleted (not rescheduled) once fired |
| `interval_s` | INTEGER | fixed-period cadence; not exposed on the model-facing `cron_set` schema and no longer written by anything — a legal column that only pre-v41 rows still use |
| `session_id` | INTEGER NOT NULL | meaning follows `target`: the pinned session (`'pin'`), else the fire anchor — the session that set the job, and for `'new'` the most recently fired one. `0` = resolve to the agent's most recently active session at fire time (its state is not a filter: a fire into a busy session queues and lands at the next turn boundary) |
| `task` | TEXT NOT NULL | injected message — the model-facing name is `prompt`; `''` = no prompt payload (with no `script` either, the fire is a bare wake) |
| `script` | TEXT | QJS path run sandboxed at fire time under the target agent's containment+grants; composes with `task` (script first). NULL = no script payload. Workspace-relative when an agent set the job (existence checked at set time, so a typo fails then rather than unattended hours later); absolute-inside-the-shared-extension-store when `extension_promote` seeded it from a manifest `scripts[]` entry. Both shapes are re-validated at fire time, and nothing else is accepted |
| `channel_name` | TEXT | chat stamp — the durable route identity a fire resolves through `channel_chats.session_id` at fire time, rather than binding a session id at set time. With `chat_id`, a composite FK → `channel_chats` (v61); both NULL = no chat. In the default target mode it is copied from the setting session's own binding (NULL for CLI/sub-agent callers); under `'new'` it is the explicit delivery target |
| `chat_id` | TEXT | with `channel_name`, the stamped chat |
| `target` | TEXT | target-mode discriminator: NULL = follow the conversation (stamped chat, else the stamped `session_id`); `'pin'` = explicit session pin; `'new'` = a fresh session per fire |
| `target_agent` | TEXT | FK → `agents(name)` ON UPDATE CASCADE (v47); agent that a `'new'`-mode fire runs under; NULL = `agent_name`. A value naming another agent is set only through an approval (see below) |
| `enabled` | INTEGER NOT NULL DEFAULT 1 | |
| `next_run_at` | INTEGER NOT NULL DEFAULT 0 | |
| `last_run_at` | INTEGER | |
| `created_at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |

Index: `idx_cron_jobs_name ON cron_jobs(agent_name, name)` — UNIQUE. The job
*name* is the logical identity (ids die with one-shot auto-removal and
delete-recreate cycles), so `cron_set` is an **upsert** by name: fields the
call names replace, the rest keep their stored value (`""` on `prompt`/
`script` clears that payload), the row is re-enabled, and validity is judged
on the *merged* row — a partial call that would leave a job with two target
modes or no schedule is refused. `cron_set` with no `name` generates one
(`job-<hex>`) and returns it. The writer is `cron_upsert()` (`src/cron.c`),
which takes the tool's JSON arguments as its document — the same document a
cross-agent approval parks and the apply path replays, so a requested job has
exactly one representation.

Rescheduling (`src/cron.c`): a one-shot (`run_at`) **deletes its row** after
firing, or after a fire that can never happen (which reports `cron_error`); a
one-shot whose fire only ran into a limit — `session_max_active`, no child
slot, its previous fire undrained — has no next schedule to retry on, so it
**stays armed** and is looked at again a minute later, with no deadline: the
limits clear by themselves. Interval and recurring jobs advance `next_run_at`
(a recurring job whose expression fails to parse at reschedule time is
disabled, never retried hourly).

### The fire path

Everything about a fire resolves at **fire time**, never set time: the durable
identities are the chat and the agent, and sessions are disposable in front of
both. `fire_due()` resolves the target, clears the guards, then dispatches the
payload.

**Target** (by `target`):

- NULL — *follow the conversation*. With a chat stamp, the fire goes to
  whatever session the chat is pinned to **right now**; with no pin for the
  chat, to the stamped `session_id`; with `session_id=0` (a CLI
  or manifest caller that never stamped one), to the agent's most recently
  active session, re-resolved every fire.
- `'pin'` — `session_id` directly. Stale-by-choice; if the session is gone the
  fire errors to the owner.
- `'new'` — a fresh session per fire, under `target_agent` (default
  `agent_name`). With channel fields it is stamped with the chat, so ordinary
  delivery reaches it; without them it is parented to the late-resolved owner
  (no `parent_tool_call_id`), so its result rides its standing parent edge
  ([delivery.md](delivery.md)) — a scheduled background `launch_agent`, no
  new delivery machinery. The route is deliberately not re-pinned (the
  accepted reply-context gap).

Authority is re-checked here, not just at set time: a chat-stamped job whose
chat now routes to a **different** agent does not fire, and says so.

**Payload** (any combination of `task`/`script`):

- prompt only — an inbox row (`source='cron'`, `source_ref=<name>`) + wake;
  it drains into a user entry and starts a turn.
- script only — a sandboxed QJS child, **no LLM call**. Its output goes
  through the same postprocess/DLP scan a tool result does, then: at a turn
  boundary (target session idle) straight in as a `type='cron_result'` entry
  followed by ordinary delivery; mid-turn as an inbox row instead, because an
  entry written mid-turn would break the mid-turn invariant.
- both — script first, then **one** inbox row carrying prompt and script
  output together, so the turn sees them as a single user entry.
- neither — a bare wake: drain whatever is already queued, annotate nothing.
  (`'new'` + bare wake creates nothing — an empty session with no payload is
  a no-op.)

The script child is a `--run-tool` fork of the js vehicle, identical to a
`js_eval` call under the target agent's `containment` and grants;
`{{SECRET:name}}` in the script file is never rewritten on disk — it resolves
in the child at the `http_request()` boundary, like any js dispatch (see
specs/security.md, JS Secret Resolution). `cron.c` decides that a script must run and hands the
decision to the daemon through the `cron_set_script_runner()` function pointer
— the child table lives in `main.c`, and this keeps the schedule with one
home instead of a queue table between them. Outside the daemon (CLI, tests)
there is no runner and a script fire reports the refusal.

**Guards**, per fire:

- *Per-job coalescing* — an undrained `source='cron'`, `source_ref=<name>` row
  on the target session skips the fire (rides `idx_inbox_pending`). One
  outstanding fire per job: "every 30 seconds" self-paces to the session's
  real consumption rate.
- *Skip-if-busy* — recurring `'new'` jobs only, the one mode coalescing cannot
  cover (each fire targets a session that did not exist yet). `session_id`
  records the last fired session; still in flight at the next fire → skip.
  In flight is the launch gate's definition (`SESSION_IN_FLIGHT`): non-idle,
  *or* idle with an unconsumed inbox row — a session that was created and has
  not started yet counts, so a job never stacks sessions behind a stall.
- *Failure auto-pause* — three consecutive failed fires set `enabled=0` and
  notify the owner with the last error (re-enable = `cron_set` on the same
  name; `cclaw --doctor` lists paused jobs). The streak is **derived** at fire
  time, not counted: a script fire's outcome is its `cron_result.is_error`, a
  prompt fire's is the last assistant `stop_reason` of the turn its drained
  entry opened, and a fire that could not resolve at all wrote a
  `source='cron_error'` inbox row. Bare wakes and both kinds of skip are
  exempt; any success resets. A turn still in flight reads as not-failed, so
  nothing is paused on incomplete evidence.

A fire that cannot happen (session gone, authority lost, script missing)
reports to the owner — the agent's most recently active session — as a
`source='cron_error'` inbox row; an agent that has never run a session gets
only a log line.

Guardrails, both in the `config` registry: `cron_min_interval_seconds`
(default 30 — one daemon tick, the finest spacing the loop can actually
deliver; enforced in `cron_schedule_check()` fire-to-fire for `cron_expr`
and delta-based for `in_seconds`, at the storage layer rather than only at
the tool boundary, since `extension_promote` is a second model-reachable
caller) and `cron_max_jobs_per_session` (default 10, enforced in the
`cron_set` tool handler only, and only for a *new* name — manifest and seeded
rows use `session_id=0` and are legitimately operator-shaped).

Targeting another agent — a `session_id` pin on a session someone else owns,
or `agent` with `session:"new"` — is an escalation: the fire would run with
that agent's grants and model. `cron_set` parks **one** approval at set time
(`tool_name='cron_set'`, `resolve='apply'`, the document in `args_json`) and
the job is written by `apply_grant()` only after a yes — a standing job, not
a per-fire prompt. Same-agent targeting, including the caller's own
sub-agent sessions, parks nothing. An explicit `channel_name`/`chat_id` is
checked against the chat's pin at set time (and again at fire).

Every agent gets a seeded, **disabled** pulse job at creation — `name='heartbeat'`,
`cron_expr='*/30 * * * *'`, no payload, `session_id=0` — written by
`cron_seed_heartbeat()` from both `agent_definition_apply` and the daemon's
`ensure_default_agent`. It is an ordinary bare-wake job in every respect; only
the seeding is special. Enabling it is a deliberate operator/agent act — a
pulse that finds queued work costs an LLM call per fire.

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

## change_log

One row per changed row of a configuration table (v60, plan v0.8 decisions
14/15), written by generated triggers — never by C — so no write path can
skip it. Logged tables and the setting/measurement class of every column are
one list, `CHANGE_LOG_COLUMNS` in `src/change_log.c`; `change_log_sync()`
generates three triggers per table (`log_<table>_ins/_upd/_del`) from it and
is run by the v60 and v61 patches and on every start (`db_ensure_schema`), so
a fresh and an upgraded database carry identical triggers. Logged now:
`agents`, `grants`, `agent_models`, `providers`, `models`, `secrets`,
`channel_chats` (v61).

| Column | Type | Notes |
|--------|------|-------|
| `id` | INTEGER PRIMARY KEY AUTOINCREMENT | |
| `at` | INTEGER NOT NULL DEFAULT (unixepoch()) | |
| `actor` | TEXT NOT NULL | `cclaw_actor()` of the writing handle: `daemon`, `cli`, `dashboard`, `sqlite3`, `llm_worker`, `tool_thread`, `llm_bridge`, `channel:<name>`, `doctor`, `channel_harness`, or `unknown` (a handle nobody named) |
| `context` | TEXT | `cclaw_context()`: what the handle was doing — `approval:<id>` while an approval is applied, `schema:v60` during that patch; NULL otherwise |
| `table_name` | TEXT NOT NULL | |
| `row_key` | TEXT NOT NULL | JSON of the primary key, after the change (before it, for a delete) |
| `action` | TEXT NOT NULL | `insert` \| `update` \| `delete` (CHECK) |
| `old_values` | TEXT | JSON of the row's settings before; NULL on insert |
| `new_values` | TEXT | JSON of the settings after; NULL on delete |

Rules the triggers implement: an UPDATE fires only `OF` setting columns and
only `WHEN` one of them actually changed (a counter update on `models` never
evaluates it; a rewrite to the same value logs nothing); an ignored `INSERT OR
IGNORE` logs nothing; an upsert logs the insert or the update that happened. A
`secret`-class column (`secrets.value`) never appears: `"(set)"` on insert,
`"(changed)"`/`"(unchanged)"` on update, absent from old values and deletes.
`INSERT OR REPLACE` would log only the insert (recursive triggers are off);
no cclaw writer uses it on a logged table. Append-only: two `BEFORE` triggers
refuse UPDATE and DELETE. A connection without `cclaw_actor()` (a system
`sqlite3`) cannot write a logged table.

---

## Design decisions

1. **Single DB** — all state in one `cclaw.db`; WAL mode allows concurrent readers; writes serialize briefly on commit; one file to back up/move.
2. **DB-driven state machine** — `advance_session()` reads DB state, decides next action; worker threads notify via pipe; no shared-memory IPC.
3. **Owner-scoped recovery** — `sessions.owner_instance` + `processes` table lets peers coexist safely; a crash orphans only that process's sessions.
4. **Split columns over JSON** — SQLite `json_object()` builds wire JSON directly from columns; metadata columns enable plan-pass queries without overflow page loads.
5. **INTEGER ids for rows, names for entities** — sessions, entries and queues use integer ids (fast joins, natural ordering); agents, extensions and channels are keyed by name (see String keys above).
6. **parent_id tree** — supports branching (Pi model); walk leaf→root via recursive CTE.
7. **Inbox/outbox as tables** — durable queues; survive crashes; atomic consumption.
8. **Agent scoping** — `agent_name` column on sessions, grants, agent_models scopes data per-agent within the shared DB; memory is scoped by attachment (`agent_memory_blocks`).
9. **Grants not config** — authority is typed grant rows; `approval_mode` + `expires_at` give fine-grained, time-bounded authority.
10. **Extensions as paths** — DB stores definition + path, never code; JS lives in files.
