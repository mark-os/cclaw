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
| Anthropic | `ANTHROPIC_API_KEY` | Direct. Different wire format (content blocks). |

Bootstrap: the env var works directly on first run; `save_secret`/admin `set key` persist the key encrypted into `secrets` (scope `system`). Resolution is env first, then the system secret under the same name. Provider rows themselves change only via the approval-gated `request_config` action `request_changes` (the `provider` section of the changes document) or operator SQL. A provider document is **transport only**; models are registered separately through the `models` section by canonical `model@provider` id, and the provider must already exist. Per-request routing joins `models → providers`, so a provider row without a models row is unreachable outside the empty-table fallback — one document can define the provider, register a model on it, and adopt it via `agent.primary_model: "model@provider"`. `providers.default_model` is fresh-install seed sugar only (`templates/seed.sql`), never a registration path.

A provider whose `api_key_env` resolves to no key (neither env var nor system-scope secret) has **all of its models skipped at request time** — `request_changes` therefore refuses such a document up front, and a canonicalized provider document preserves an existing row's `api_key_env` verbatim, empty string included (a deliberately keyless local gateway must survive a provider edit).

The skip is no longer silent. A candidate named by an agent's
`primary_model`/`secondary_model` (or by the `default_*_model` config keys) and
dropped for an unresolvable key logs at WARN and is marked **degraded by
configuration**: `models.status='degraded'` with `degraded_until` left NULL.
That state is set directly on the healthy→degraded transition — no error
counts are faked, because no request happened — which is exactly what makes the
existing one-shot operator notice fire once and only once. The NULL cooldown is
the marker distinguishing it from every error-driven degradation (those always
set one), and it is what lets routing restore `healthy` the moment the key
resolves again, mirroring `model_stat_success`. This is the operator-visible
half of the same defect the apply-time probe covers for the agent
(see [self-configuration.md](self-configuration.md)).

## Provider Fallback Chain

The `providers` table rows ordered by `priority` — row 0 is primary, the rest are the fallback chain. On 5xx/timeout from the primary, the next row is tried. Fallback API keys resolve the same way as the primary: env var named by `api_key_env`, then the encrypted `secrets` row (scope `system`) under that name.

## Wire Format Differences

CClaw speaks OpenAI Completions format. OpenRouter normalizes everything.

| Provider | Format | Tool Call Style | Tool Result Style |
|----------|--------|----------------|-------------------|
| OpenAI/OR | `choices[0].message` | `tool_calls[].function.arguments` (stringified) | `role: "tool"` |
| Anthropic | Content blocks | `type: "tool_use", input: {}` (object) | Wrapped in user message |
| Google | `Content[].parts` | `functionCall.args: {}` (object) | `functionResponse` in user |

Key: OpenAI is the odd one out — `arguments` is a string. Everyone else uses objects.
CClaw stores `args` as object in `tool_calls` column (provider-neutral). OpenAI emitter stringifies at wire time.

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
