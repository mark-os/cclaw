# Provider Auth & Access

## Provider Config in cclaw.db

All provider configuration lives in `cclaw.db`:
- `providers` table: name, base_url, endpoint_type, api_key_env — **transport only**
- `models` table: canonical id (`model@provider`), context_window, max_output_tokens, capabilities, routing priority/status
- `secrets` table (scope `system`): encrypted API keys, named by the provider's `api_key_env` (e.g. `OPENROUTER_API_KEY`)

Agents ⊥ store provider keys. Keys decrypted at runtime, injected to worker threads and tool children via env vars.

## Supported Providers

| Provider | Env Var (bootstrap) | Notes |
|----------|---------------------|-------|
| OpenRouter | `OPENROUTER_API_KEY` | Default. Routes to any model. Normalizes to OpenAI format. |
| Gemini | `GEMINI_API_KEY` | Direct. Sent as `x-goog-api-key` header. |
| DeepSeek | `DEEPSEEK_API_KEY` | Direct. Cheapest for DeepSeek models. |
| OpenAI | `OPENAI_API_KEY` | Direct. |
| Cerebras | `CEREBRAS_API_KEY` | Direct, OpenAI-compat. Wants `max_tokens` (not `max_completion_tokens`) — which is already what CClaw sends. `reasoning_effort` values differ per model family, so the effort mapping is per-model, not global. |
| Anthropic | `ANTHROPIC_API_KEY` | **Not implemented.** Anthropic's native API is a different wire format (top-level `system`, content blocks, `tool_use`/`tool_result`) and would need a third builder in `llm_payload.c`. Scoped out 2026-08-20; reach Claude models via OpenRouter until then. |

Bootstrap: the env var works directly on first run; `save_secret`/admin `set key` persist the key encrypted into `secrets` (scope `system`). Resolution is env first, then the system secret under the same name. Provider rows themselves change only via the approval-gated `request_config` action `request_changes` (the `provider` section of the changes document) or operator SQL. A provider document is **transport only**; models are registered separately through the `models` section by canonical `model@provider` id, and the provider must already exist. Per-request routing joins `models → providers`, so a provider row without a models row is unreachable outside the empty-table fallback — one document can define the provider, register a model on it, and adopt it via `agent.models: ["model@provider", ...]` (the full replacement routing order). `providers.default_model` is fresh-install seed sugar only (`templates/seed.sql`), never a registration path.

A provider whose `api_key_env` resolves to no key (neither env var nor system-scope secret) has **all of its models skipped at request time** — `request_changes` therefore refuses such a document up front, and a canonicalized provider document preserves an existing row's `api_key_env` verbatim, empty string included (a deliberately keyless local gateway must survive a provider edit).

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

## Provider Fallback Chain

The `providers` table rows ordered by `priority` — row 0 is primary, the rest are the fallback chain. On 5xx/timeout from the primary, the next row is tried. Fallback API keys resolve the same way as the primary: env var named by `api_key_env`, then the encrypted `secrets` row (scope `system`) under that name.

## Wire Format Differences

CClaw implements **two** request builders (`src/llm_payload.c`): OpenAI-compat
chat completions (OpenRouter, OpenAI, DeepSeek, Cerebras, …) and Gemini-native
(`generateContent`). Anthropic's native format is **not** implemented — it is
future work requiring a third builder. Everything else reaches CClaw through
OpenRouter's normalization.

| Provider | Format | Tool Call Style | Tool Result Style |
|----------|--------|----------------|-------------------|
| OpenAI/OR | `choices[0].message` | `tool_calls[].function.arguments` (stringified) | `role: "tool"` |
| Google | `Content[].parts` | `functionCall.args: {}` (object) | `functionResponse` in user |
| Anthropic *(not implemented)* | Content blocks | `type: "tool_use", input: {}` (object) | Wrapped in user message |

**System-role convention (both builders)**: only the leading system prompt
carries the system role (`messages[0]` / `systemInstruction`). All other
system-originated content — mid-turn `type='system'` entries, compaction
summaries, hook injects — is emitted as a `'[system] '`-prefixed **user**
message on every provider. No builder ever emits a non-leading `role: system`.

Key: OpenAI is the odd one out — `arguments` is a string. Everyone else uses objects.
CClaw stores `args` as object in `tool_calls` column (provider-neutral). OpenAI emitter stringifies at wire time.

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
level name.

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
- Decrypted at startup → loaded into config, available to worker threads
- Agent uses key for LLM calls, key lives only in process memory
- `shell_exec` children have provider-native env vars (e.g. `OPENROUTER_API_KEY`) unset before exec
- Agent ⊥ has access to `.cclaw_key` file (daemon-only, not exposed to agents)
- Provider changes are approval-gated: the `provider` section of `request_config` action `request_changes` parks for human approval; the upsert happens in the trusted parent on approve (savepoint-wrapped, all-or-nothing with any co-batched grants/config). The approval args carry only the secret's *name* (`api_key_env`) — never key material. (The old `configure_provider` tool applied inline, letting a granted agent silently repoint `base_url` at an attacker — deleted, schema v28.)

## Future: OAuth (Device Code)

If CClaw needs subscription-based access (ChatGPT Plus, Claude Pro):
- OAuth 2.0 Device Authorization Grant (RFC 8628)
- Works headless: show URL + code, user approves on phone/laptop
- Tokens stored encrypted in the `secrets` table (scope `system`)
- Google bans accounts for third-party OAuth — use API key only
