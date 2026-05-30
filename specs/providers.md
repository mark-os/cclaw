# Provider Auth & Access

## Provider Config in cclaw.db

All provider configuration lives in `cclaw.db`:
- `providers` table: name, base_url, model, context_window
- `kv` table: encrypted API keys (`enc:` prefix), fallback config

Agents ⊥ store provider keys. Daemon decrypts at fork, injects via `provider-native env var (e.g. OPENROUTER_API_KEY)` env var.

## Supported Providers

| Provider | Env Var (bootstrap) | Notes |
|----------|---------------------|-------|
| OpenRouter | `OPENROUTER_API_KEY` | Default. Routes to any model. Normalizes to OpenAI format. |
| Gemini | `GEMINI_API_KEY` | Direct. Sent as `x-goog-api-key` header. |
| DeepSeek | `DEEPSEEK_API_KEY` | Direct. Cheapest for DeepSeek models. |
| OpenAI | `OPENAI_API_KEY` | Direct. |
| Anthropic | `ANTHROPIC_API_KEY` | Direct. Different wire format (content blocks). |

Bootstrap: env var seeds `kv` in cclaw.db on first run. After that, cclaw.db is authoritative.

## Provider Fallback Chain (T45)

Stored in cclaw.db `kv` as `fallback_providers` (JSON array). On 5xx/timeout from primary, try next:
```json
[{"name": "gemini", "base_url": "https://generativelanguage.googleapis.com/v1beta/openai", "model": "gemma-4-31b-it"}]
```

API keys for fallback providers also in cclaw.db `kv` (encrypted).

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

- Provider API keys stored encrypted in cclaw.db `kv` (ChaCha20-Poly1305 AEAD)
- Decryption key: `.cclaw/.cclaw_key` (32 bytes, mode 0600, daemon-only)
- Daemon decrypts at fork → injects as `provider-native env var (e.g. OPENROUTER_API_KEY)` env var
- Agent uses key for LLM calls, key lives only in process memory
- `shell_exec` children have `provider-native env var (e.g. OPENROUTER_API_KEY)` unset before exec (V47)
- Agent ⊥ has access to `.cclaw_key` file (daemon-only, not exposed to agents)
- `configure_provider` tool (bootstrap): agent exits w/ code 4, daemon stores encrypted key

## Future: OAuth (Device Code)

If CClaw needs subscription-based access (ChatGPT Plus, Claude Pro):
- OAuth 2.0 Device Authorization Grant (RFC 8628)
- Works headless: show URL + code, user approves on phone/laptop
- Tokens stored encrypted in cclaw.db `kv`
- Google bans accounts for third-party OAuth — use API key only
