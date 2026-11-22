# Provider Auth & Access

## Provider Config in cclaw.db

All provider configuration lives in `cclaw.db`:
- `providers` table: name, base_url, endpoint_type, api_key_env — **transport only**
- `models` table: canonical id (`model@provider`), context_window, max_output_tokens, capabilities, effort_map, health status
- `agent_models` table: each agent's routing lists, one per `purpose` (`chat`, `image`, `audio`, `utility`), each in `pos` order
- `secrets` table (scope `system`): encrypted API keys, named by the provider's `api_key_env` (e.g. `OPENROUTER_API_KEY`)

Agents never store provider keys. The trusted process decrypts a key when it loads config and uses it on its LLM worker threads. Provider keys never reach tool children (see Security Model below).

## Supported Providers

| Provider | Env Var (bootstrap) | Notes |
|----------|---------------------|-------|
| OpenRouter | `OPENROUTER_API_KEY` | Default. Routes to any model. Normalizes to OpenAI format. |
| Gemini | `GEMINI_API_KEY` | Direct. Sent as `x-goog-api-key` header. |
| DeepSeek | `DEEPSEEK_API_KEY` | Direct. Cheapest for DeepSeek models. |
| OpenAI | `OPENAI_API_KEY` | Direct. |
| Cerebras | `CEREBRAS_API_KEY` | Direct, OpenAI-compat. Wants `max_tokens` (not `max_completion_tokens`) — which is already what CClaw sends. `reasoning_effort` values differ per model family, so the effort mapping is per-model, not global. |
| Anthropic | `ANTHROPIC_API_KEY` | Direct, native Messages API (`endpoint_type='anthropic'`). Sent as `x-api-key`. Seeded with `anthropic/claude-sonnet-5-5`. See [Anthropic Messages](#anthropic-messages). |

Bootstrap: the env var works directly on first run; the first-run prompt, `cclaw secret set <NAME>` (for a name some provider uses) and the admin page persist the key encrypted into `secrets` (scope `system`). An agent cannot write a system secret directly: it captures the key with `save_secret` (agent scope, granted to itself, value never in context) and names it in a provider document, and approving that document moves the secret to system scope and deletes its grants. Only a secret granted to the asking agent and bound to no host qualifies, so a tool credential cannot be carried off to a new address this way; the approval prompt says the secret becomes the provider's key. Resolution is env first, then the system secret under the same name. Provider rows change only through the `provider` section of an approval-gated `request_config` changes document ([config-doc.md](config-doc.md)), or through operator SQL. A provider document is **transport only**; models are registered separately through the `models` section by canonical `model@provider` id, and the provider must already exist. Per-request routing joins `models → providers`, so a provider row without a models row is unreachable outside the empty-table fallback — one document can define the provider, register a model on it, and adopt it via `agent.models: ["model@provider", ...]` (the full replacement routing order). `providers.default_model` is fresh-install seed sugar only (`templates/seed.sql`), never a registration path.

A provider whose `api_key_env` resolves to no key (neither env var nor system-scope secret) has **all of its models skipped at request time** — `request_config` therefore refuses such a document up front, and a canonicalized provider document preserves an existing row's `api_key_env` verbatim, empty string included (a deliberately keyless local gateway must survive a provider edit).

The skip is no longer silent. A candidate named by an agent's
routing list (`agent_models`) and dropped for an unresolvable key logs at WARN
and is marked **degraded by configuration**: `models.status='degraded'` with
`degraded_until` left NULL. That state is set directly on the
healthy→degraded transition — no failure counts are faked, because no request
happened. The NULL cooldown is the marker distinguishing it from every
error-driven degradation (those always set one), and it is what lets routing
restore `healthy` the moment the key resolves again, mirroring
`model_stat_success`. The operator-visible signal for any of this is the
serving-model-change notice (specs/error-handling.md). This is the operator-visible
half of the same defect the apply-time probe covers for the agent
(see [self-configuration.md](self-configuration.md)).

### The seeded OpenRouter provider

A new install talks to OpenRouter with `deepseek/deepseek-v4.1-flash` and this
`providers.request_extra`:

```json
{"provider": {"ignore": ["StreamLake"], "zdr": true}}
```

- `zdr: true` restricts routing to endpoints with a Zero Data Retention
  policy (OpenRouter's request-level switch; it can only tighten the account's
  own setting). OpenRouter does not count in-memory prompt caching as
  retention, so cached-prefix pricing still applies. A model with no such
  endpoint fails with OpenRouter's error and the routing list moves on; to
  use one anyway, the operator edits `request_extra` for the provider.
- `ignore: ["StreamLake"]` routes around an upstream that returned tool calls
  as markup.

The seed is `INSERT OR IGNORE`: an existing install keeps its provider row,
model and routing list as they are.

## Provider Fallback Chain

Routing does not walk the `providers` table. Each request walks the agent's
`agent_models` list in `pos` order (`load_candidates`, `src/llm_proc.c`).
Health reorders that list and a failure moves to the next candidate; the
policy is in [error-handling.md](error-handling.md). A candidate whose
provider key does not resolve drops out, as described above. Chat routing
reads only the `purpose='chat'` rows.

### Media models (image / audio)

Inbound media (a voice note, a photo) is always turned into text by a
separate model before the agent sees it. Which model: the session's agent's
`agent_models` rows for that purpose (`image` or `audio`), in `pos` order
(`model_pick_for_media`, `src/llm_proc.c`). An agent with no rows for the
purpose gets the global pick: every non-disabled model whose `capabilities`
contain the purpose and that is not in an error cooldown, in catalog order
(`model_pick_by_capability`). With rows, the list is the whole policy, the
same rule as chat: a model in its cooldown sinks below the healthy ones but is
still tried, `disabled` drops out, and the global pick is never appended — so
media is never sent to a model nobody chose for that agent. A row is only
accepted for a model whose `capabilities` include the purpose (enforced by a
trigger on `agent_models`). There is no agent-facing way to set these lists
yet; the operator writes the rows.

### Utility models

The `utility` list is for work an extension's script hands to a model with
`LLM(prompt, {purpose: "utility"})` ([js-llm.md](js-llm.md#purpose)) — the
post-turn review in `extensions/turn-review` is the first user. It lets an
agent think with one model and tidy up with a cheaper one. The list follows
the chat rules (pos order, cooldowns sink, `disabled` drops out, any model,
optional `reasoning_effort`). An agent with no `utility` rows is served from
its chat list; one whose utility rows are all unroutable gets an error, not
the chat list. Like the media lists it is the operator's to write — the
explorer's agent page ("Utility"), or:

```sql
INSERT INTO agent_models(agent_name, purpose, model_id, pos)
VALUES ('Assistant', 'utility', 'anthropic/claude-haiku-5-5', 0);
```

`providers.priority` has one remaining use: at config load, the first
provider in `priority` order whose key resolves becomes `cfg->provider`.
`llm_req()` uses it only for the synthetic candidate it builds when the agent
has no routable model row. Every key resolves the same way: the env var named
by `api_key_env`, then the encrypted `secrets` row (scope `system`) under that
name.

## Wire Format Differences

CClaw implements **three** request builders (`src/llm_payload.c`), selected by
`providers.endpoint_type`: `openai` — OpenAI-compat chat completions
(OpenRouter, OpenAI, DeepSeek, Cerebras, …); `gemini` — Gemini-native
`generateContent`; `anthropic` — the Anthropic Messages API. Everything else
reaches CClaw through OpenRouter's normalization.

| Provider | Format | Tool Call Style | Tool Result Style |
|----------|--------|----------------|-------------------|
| OpenAI/OR | `choices[0].message` | `tool_calls[].function.arguments` (stringified) | `role: "tool"` |
| Google | `Content[].parts` | `functionCall.args: {}` (object) | `functionResponse` in user |
| Anthropic | `content[]` blocks | `type: "tool_use", input: {}` (object) | `tool_result` blocks, one user message per iteration |

**System-role convention (all builders)**: only the leading system prompt
carries the system role (`messages[0]` / `systemInstruction` / top-level
`system`). All other
system-originated content is emitted as a **user** message on every provider:
mid-turn `type='system'` entries with a `'[system] '` prefix, compaction
summaries with `'[Summary of earlier conversation]'`. No builder ever emits a
non-leading `role: system`. The prefixes are applied once, in the transcript
step below.

Key: OpenAI is the odd one out — `arguments` is a string. Everyone else uses objects.
A tool call's arguments are stored once, as JSON text in the `tool_call`
entry's `content`; the OpenAI projection sends that string, the other two
send it as an object.

### Payload assembly: plan → transcript → wire

Every request is built in three steps, all in SQL (`llm_build_payload`):

1. **Plan** — `context_plan()` picks the entries on the branch that fit the
   budget; their ids go into the temp table `_plan(pos, entry_id)`.
2. **Transcript** — one statement turns the plan into the temp table
   `_transcript`, one row per part, in a provider-neutral form. Everything the
   three formats agree on is decided here and nowhere else:
   - which entries are sent, and each one's role (`user` / `assistant` /
     `tool`) and kind (`text` / `tool_call` / `tool_result`);
   - text decoration: the `'[system] '` and `'[Summary of earlier
     conversation]'` prefixes, the `cron_result` label, the untrusted-content
     wrap on tool results;
   - grouping (`grp`): an iteration's assistant text and tool calls share a
     key, that iteration's tool results share another, every other row has its
     own;
   - reasoning: each assistant row carries the id of its iteration's
     `reasoning` entry **if that entry is on the plan**;
   - the user context block, as a row of its own (below).
3. **Wire** — `SQL_OPENAI_MESSAGES`, `SQL_GEMINI_CONTENTS` and
   `SQL_ANTHROPIC_MESSAGES` each project `_transcript` into their shape. What
   stays per format is what genuinely differs: how groups become messages
   (OpenAI and Gemini send each tool result separately, Anthropic sends an
   iteration's results as one user message), the reasoning replay artifact,
   cache markers, empty-text guards.

The wire queries read the conversation only through `_transcript`, so **only
what is on the plan is sent**: a tool call, tool result or reasoning entry
that has been taken off the branch (the row still exists) appears on no
provider, and an iteration with neither text nor a tool call on the plan sends
no message. Both temp tables are dropped by `llm_payload_release()`; the
request body itself comes zero-copy from a statement that reads neither.

### The user context block

The per-turn `<RELEVANT_CONTEXT>` text (clock, recall, context-placement
memory, running sub-agents, background jobs, pending approvals) is called the
**user context block**. It is built once at turn start, stored in
`sessions.turn_context`, and sent as a user message of its own.

**Position**: immediately before the first entry of the newest turn on the
plan (newest = highest `entries.turn_id`). A turn that opens with several
queued user messages gets the block before all of them. It does not move while
the turn runs, so every iteration of a turn extends the same prompt-cache
prefix, and it never lands between an assistant's tool calls and their
results. Entries that do not open a turn do not move it: an edit's summary
carries the turn id of what it replaced, a `cron_result` the turn it lands
in. If no entry on the plan has a `turn_id`, the block goes before the first
entry.

## Anthropic Messages

`POST <base_url>/messages` with `x-api-key`, `anthropic-version: 2026-10-21`
and `anthropic-beta: thinking-binding-controls-2026-08-01`
(`llm_build_headers`, `src/llm_transport.c`). A `providers` row plus a `models`
row is all the setup there is; the seed ships both, inert until
`ANTHROPIC_API_KEY` resolves:

```sql
-- route an agent to it (or use a request_config document's agent.models)
UPDATE agent_models SET model_id='anthropic/claude-sonnet-5-5'
 WHERE agent_name='Assistant' AND purpose='chat' AND pos=0;
```

What the builder does that the other two don't, and why:

- **`max_tokens` is always sent, floor 16000.** The API requires it, and on
  current models it also pays for thinking, which cannot be switched off — the
  global 4096 default would regularly be spent before any visible text.
  `models.max_output_tokens`, when set on the routed row, wins.
- **Thinking is replayed verbatim, but only inside the turn that produced
  it.** A thinking block's signature covers everything before it — `system`,
  `tools`, every earlier message — and the block must come back in its
  original position, so what is stored (`reasoning_meta.format =
  'anthropic_content'`) and replayed is the response's whole `content` array.
  CClaw's user context block moves at every turn boundary, which is an edit
  to that prefix; inside a turn the prefix is byte-stable until a session
  edit between two iterations changes it. So blocks from earlier turns are
  dropped (a leading run, which the API permits), and the current turn's go
  back untouched — except those an edit overtook: blocks produced before an
  edit and standing at or after its `thinking_cut` are stale and not
  replayed (a trailing run), and the model re-thinks from there
  ([turn-edit.md](turn-edit.md#thinking-after-an-edit)). A model switch
  falls back to rebuilding the message from entries, without thinking, and so
  does an iteration whose `reasoning` entry is not on the plan.
- **`thinking.block_binding.prefix_mismatch_behavior: "drop_block"`.** On
  accounts where the binding check is enforced, replaying a block after its
  prefix changed is a 400. The scoping above should prevent that; this is
  the backstop, and losing one block's reasoning beats losing the turn. Every drop is
  reported in the response's `input_transformations`, archived with the body:

  ```sql
  SELECT id, json_extract(body,'$.input_transformations') FROM llm_responses
   WHERE json_array_length(body,'$.input_transformations') > 0;
  ```
- **Prompt caching is opt-in, so the builder opts in** with four markers (the
  API's limit): the system prompt; the request-level marker, which follows the
  growing tail inside a turn; and the last message before the start of each
  of the two newest turns. That last pair exists because of the same moving
  user context block — the stable prefix ends just before it, so one marker
  writes the entry the next turn reads and the other reads the entry the
  previous turn wrote. `llm_responses.cached_tokens` / `cache_write_tokens` show it
  working, and the explorer's cache view (`#/cache`) shows the hit rate by
  the gap between requests. Changing an agent's tool set invalidates
  everything (tools are the head of the prefix).
- **The marker's lifetime is the routing entry's `cache_lifetime`.** 3600
  sends `{"type":"ephemeral","ttl":"1h"}` on every marker; 300, or none, the
  plain `{"type":"ephemeral"}`, which is the 5-minute cache. All markers move
  together, since the API refuses a longer-lived marker after a shorter one.
  No beta header is needed. On an Anthropic-format model the value is 300 or
  3600 (triggers on `agent_models`), and a new `chat` entry that gives none
  gets 3600, the lifetime that matches an agent that waits on people. The
  cost: a 1-hour write is billed at 2× base input instead of 1.25× (reported
  in `cache_creation.ephemeral_1h_input_tokens`, which the stored
  `cache_write_tokens` already sums). Set 300 on an
  entry whose sessions run back-to-back (a busy sub-agent) to pay the lower
  write price. The same value arms `CacheExpiring`
  ([hooks.md](hooks.md#cacheexpiring)).
- **`thinking.display` follows `reasoning_display`**:
  `summarized` for `summary`, `omitted` for `none`, always sent explicitly
  ([below](#reasoning-display)).
- **Usage is normalized.** Anthropic's `input_tokens` excludes cache reads and
  writes; the stored prompt count adds them back, so `usage_in` means the same
  thing on every provider.
- **A refusal** (`stop_reason: "refusal"`, HTTP 200) becomes an error-stop
  assistant entry whose text names the category. No server-side fallback is
  requested; the routing list is CClaw's fallback mechanism.
- **No sampling parameters.** This API version rejects `temperature` at any
  value, so `LLM()`'s `opts.temperature` is not forwarded on this format.
- **No audio.** A media job needing transcription skips an Anthropic
  candidate; images work.

`providers.request_extra` remains the last word, and RFC 7386 `null` removes a
key, which covers the model-specific cases without code:

| Want | `request_extra` |
|------|-----------------|
| A model that predates adaptive thinking (Haiku 4.5 and older) | `{"thinking": null}` |
| No prompt caching | `{"cache_control": null}` (the per-block markers remain) |

## Reasoning Display

Whether the model is asked for a readable summary of its reasoning is also
a routing decision: `agent_models.reasoning_display` ∈ `summary` (the
default) | `none`. The text is kept as the `reasoning` entry's `content`,
shown in the explorer, and sent to hook scripts as each iteration's
`reasoning`; the replay blob (`reasoning_meta`) is captured either way, and
token accounting does not change.

| Wire format | `summary` | `none` |
|---|---|---|
| Anthropic | `thinking.display: "summarized"` — readable text in each thinking block, kept | `thinking.display: "omitted"` (empty text) |
| Gemini native | `generationConfig.thinkingConfig.includeThoughts: true` — thought summaries come back as `thought: true` parts, kept as the text and never replayed | nothing sent |
| OpenAI-compatible | nothing sent; whatever reasoning text the provider returns is kept | nothing sent; returned text is not stored |

Display changes visibility only: the signature, the billing and the replay
are the same, and a summarized Anthropic block replays verbatim. On Gemini
the signature a replayed `functionCall` needs rides on the call part either
way. The Gemini patch comes before the effort fragment, so a level's
`thinkingConfig` merges beside it, and `request_extra` keeps the last word.

## Reasoning Effort

How hard a model is asked to think is a **routing** decision, so the level
lives on the routing entry: `agent_models.reasoning_effort` ∈
`off | minimal | low | medium | high`. NULL — the default, and what every
upgraded DB starts with — sends nothing at all, byte for byte the payload
CClaw built before the knob existed.

How that level is *spelled* is a **per-model** property, so it lives on
`models.effort_map`. Per-provider would be wrong: Cerebras' gpt-oss takes
low/medium/high while its qwen3 takes neither, and Gemini 2.5 wants a token
budget where 3.x wants a level.

```json
{"format": "openrouter",
 "levels": {"off": "off", "low": "low", "medium": "medium", "high": "high"}}
```

Level values are **wire values** — strings for effort enums, integers for
budgets. A level that is missing or `null` is *unsupported by this model*.

| `format` | Emits |
|----------|-------|
| `openrouter` | `{"reasoning": {"effort": <value>}}` — `off` becomes `{"reasoning": {"enabled": false}}`, since OpenRouter has no `"off"` effort |
| `openai` | `{"reasoning_effort": <value>}` (flat; also the Cerebras shape) |
| `deepseek` | `{"thinking": {"type": "enabled"\|"disabled"}}` — driven by the level *name* after clamping (`off` → disabled, anything else → enabled); the map's values only mark which levels exist |
| `gemini-level` | `generationConfig.thinkingConfig.thinkingLevel` (string) |
| `gemini-budget` | `generationConfig.thinkingConfig.thinkingBudget` (integer) |
| `anthropic` | `{"output_config": {"effort": <value>}}` |

An unrecognized `format` falls back to `openrouter`.

**Clamp to nearest supported** (pi's rule). If the requested level has no
value in the map, CClaw walks the ladder `off < minimal < low < medium < high`
**upward first**, then downward, and uses the nearest level that does — more
thinking beats silently less, and ties break upward. Requesting `high` from a
map that tops out at `medium` sends medium; requesting `low` from a map that
only supports `high` sends high. A map that supports **no** level sends
nothing.

**No `effort_map`** on the model: the endpoint's own vocabulary, identity-
mapped. OpenAI-compat gets `openrouter` format (that traffic is overwhelmingly
OpenRouter, which converts its unified `reasoning.effort` to whatever the
backend wants); the Gemini endpoint gets `gemini-level` with the uppercased
level name; the Anthropic endpoint gets `anthropic` with `minimal` and `low`
both sending `low` and no `off` (current models always think, so `off` clamps
up). Anthropic's `xhigh` and `max` sit above CClaw's ladder — a model row's own
map can point `high` at one of them.

**Precedence.** The mapped fragment is merged into the request body *before*
`providers.request_extra`, so a provider's `request_extra` still overrides or
suppresses it — that column remains the last word on the wire.

**Setting it.** `reasoning_effort` is agent-settable through the approval-gated
`request_config` document: an `agent.models` entry may be an `{id, effort}`
object instead of a bare id string (see
[config-doc.md](config-doc.md)). `models.effort_map` is deliberately NOT on
that surface — how a level spells on the wire is a fact about the model, not an
agent preference — so operators write it directly (`db_query` / SQL):

```sql
UPDATE models SET effort_map='{"format":"gemini-budget",
  "levels":{"low":1024,"medium":8192,"high":24576}}'
 WHERE id='gemini-2.5-pro@openrouter';
```

## Security Model

- Provider API keys stored encrypted in the `secrets` table with scope `system` (ChaCha20-Poly1305 AEAD). System scope is excluded from the agent-facing snapshot: `{{SECRET:OPENROUTER_API_KEY}}` does not resolve, and the key is never injected into tool children.
- Decryption key: `<dir of cclaw.db>/.cclaw_key` (32 bytes, mode 0600, daemon-only)
- Decrypted by the trusted process and held only in its memory; used by LLM worker threads
- `shell_exec` children have provider-native env vars (e.g. `OPENROUTER_API_KEY`) unset before exec
- Agents have no access to the `.cclaw_key` file: every sandboxed profile masks it
- Provider changes are approval-gated: a `request_config` document with a `provider` section parks for human approval; the upsert happens in the trusted parent on approve (savepoint-wrapped, all-or-nothing with any co-batched grants/config). The approval args carry only the secret's *name* (`api_key_env`) — never key material. (The old `configure_provider` tool applied inline, letting a granted agent silently repoint `base_url` at an attacker — deleted, schema v28.)

## Future: OAuth (Device Code)

Not planned. The survey and the current decision (no subscription
integration for Claude) are in
[subscription-providers.md](subscription-providers.md). If subscription-based
access is revisited:
- OAuth 2.0 Device Authorization Grant (RFC 8628)
- Works headless: show URL + code, user approves on phone/laptop
- Tokens stored encrypted in the `secrets` table (scope `system`)
- Google bans accounts for third-party OAuth — use API key only
