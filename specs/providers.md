# Provider Auth & Access

## Provider Config in cclaw.db

All provider configuration lives in `cclaw.db`:
- `providers` table: name, base_url, model, context_window
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

Bootstrap: the env var works directly on first run; `save_secret`/admin `set key` persist the key encrypted into `secrets` (scope `system`). Resolution is env first, then the system secret under the same name. Provider rows themselves change only via the approval-gated `request_config` action `request_changes` (the `provider` section of the changes document) or operator SQL. Applying a provider section also seeds its default model into `models` (id `model@provider`, lowest routing priority) — per-request routing joins `models → providers`, so a provider row without a models row would otherwise be unreachable outside the empty-table fallback. An agent adopts the new provider by co-batching `agent.primary_model: "model@provider"` in the same document.

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
