# Provider Auth & Access

## Current: API Keys Only

CClaw uses env vars or config.json for all provider auth. No OAuth flows.

| Provider | Env Var | Notes |
|----------|---------|-------|
| OpenRouter | `OPENROUTER_API_KEY` | Default. Routes to any model. Normalizes to OpenAI format. |
| Gemini | `GEMINI_API_KEY` | Direct. Sent as `x-goog-api-key` header. |
| DeepSeek | `DEEPSEEK_API_KEY` | Direct. Cheapest for DeepSeek models (no OR markup). China-based. |
| OpenAI | `OPENAI_API_KEY` | Direct. |
| Anthropic | `ANTHROPIC_API_KEY` | Direct. Different wire format (content blocks). |

## Provider Fallback Chain (T45)

Config array of providers. On 5xx/timeout from primary, try next:
```json
"fallback_providers": [
  {"base_url": "https://generativelanguage.googleapis.com/v1beta/openai", "api_key": "$GEMINI_API_KEY", "model": "gemma-4-31b-it"}
]
```

## Wire Format Differences

CClaw currently speaks only OpenAI Completions format. OpenRouter normalizes everything.

| Provider | Format | Tool Call Style | Tool Result Style |
|----------|--------|----------------|-------------------|
| OpenAI/OR | `choices[0].message` | `tool_calls[].function.arguments` (stringified) | `role: "tool"` |
| Anthropic | Content blocks | `type: "tool_use", input: {}` (object) | Wrapped in user message |
| Google | `Content[].parts` | `functionCall.args: {}` (object) | `functionResponse` in user |
| Bedrock | SDK events | `toolUse.input: {}` (object) | `toolResult` in user |

Key: OpenAI is the odd one out — `arguments` is a string. Everyone else uses objects.
CClaw stores `args` as object in `tool_calls` column (provider-neutral). OpenAI emitter stringifies at wire time.

## Future: OAuth (Device Code)

If CClaw needs subscription-based access (ChatGPT Plus, Claude Pro):
- OAuth 2.0 Device Authorization Grant (RFC 8628)
- Works headless: show URL + code, user approves on phone/laptop
- No browser callback needed
- OpenAI explicitly supports this (Codex product)
- Google bans accounts for third-party OAuth — use API key only

## Security Notes

- Keys entered via Telegram admin dialog → written to config file, ⊥ stored in DB/session/LLM context (V52)
- `shell_exec` children have API key env vars unset (V47)
- Keys in config.json are plaintext (no encryption layer — honest approach)
