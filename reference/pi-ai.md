# Pi Agent — AI/LLM Layer Reference

## Provider Architecture

### Registry Pattern
- Global `Map<string, RegisteredApiProvider>` keyed by API type string
- Each provider registers `{ api, stream, streamSimple }`
- Lazy-loaded on first use

### Wire Protocol Families (API Types)
| API Type | Used By |
|----------|---------|
| `openai-completions` | OpenAI, DeepSeek, xAI, Groq, Cerebras, OpenRouter, Together, Cloudflare, etc. |
| `openai-responses` | OpenAI Responses API (newer event/items format) |
| `openai-codex-responses` | OpenAI Codex (WebSocket support) |
| `azure-openai-responses` | Azure-hosted OpenAI |
| `anthropic-messages` | Anthropic Messages API |
| `bedrock-converse-stream` | AWS Bedrock ConverseStream |
| `google-generative-ai` | Google Gemini |
| `google-vertex` | Google Vertex AI |
| `mistral-conversations` | Mistral |

### Model Definition
```
{
  id, name, api, provider, baseUrl,
  reasoning: boolean,
  thinkingLevelMap?: maps pi levels to provider values,
  input: ("text"|"image")[],
  cost: { input, output, cacheRead, cacheWrite },  // $/M tokens
  contextWindow, maxTokens,
  headers?: Record<string, string>,
  compat?: provider-specific overrides
}
```

---

## Wire Format Details

### OpenAI Chat Completions

**Request:**
```json
{
  "model": "...",
  "messages": [...],
  "tools": [...],
  "max_tokens": N,
  "logprobs": true,
  "top_logprobs": N
}
```

**Response fields:**
- `choices[0].message.content` — text
- `choices[0].message.tool_calls` — array of `{id, function: {name, arguments}}`
- `choices[0].finish_reason` — `stop|length|tool_calls|content_filter`
- `choices[0].logprobs` — per-token log probabilities
- `usage.{prompt_tokens, completion_tokens, total_tokens}`
- `usage.prompt_tokens_details.{cached_tokens, cache_write_tokens}`
- `usage.completion_tokens_details.{reasoning_tokens}`
- `usage.prompt_cache_hit_tokens` — DeepSeek direct

**Reasoning field variants (provider-dependent):**
- `message.reasoning` — OpenRouter, generic
- `message.reasoning_content` — DeepSeek, llama.cpp
- `message.reasoning_text` — some providers
- `reasoning_details[].data` — encrypted reasoning (OpenAI)

**Thinking configuration variants:**
| Provider | Format |
|----------|--------|
| OpenAI | `reasoning_effort` top-level |
| DeepSeek | `thinking: {type: "enabled"}` + `reasoning_effort` |
| OpenRouter | `reasoning: {effort: "..."}` |
| Together | `reasoning: {enabled: bool}` + optional `reasoning_effort` |
| z.ai | `enable_thinking: boolean` top-level |
| Qwen | `enable_thinking: boolean` top-level |
| Qwen (chat-template) | `chat_template_kwargs.enable_thinking` |

**Compat flags (17+ booleans):**
`supportsStore, supportsDeveloperRole, maxTokensField, requiresToolResultName, requiresThinkingAsText, sendSessionAffinityHeaders, cacheControlFormat, ...`

### Anthropic Messages API

**SSE events:** `message_start`, `content_block_start`, `content_block_delta`, `content_block_stop`, `message_delta`, `message_stop`

**Content block types:** `text`, `thinking`, `redacted_thinking`, `tool_use`

**Delta types:** `text_delta`, `thinking_delta`, `input_json_delta`, `signature_delta`

**Stop reasons:** `end_turn`→stop, `max_tokens`→length, `tool_use`→toolUse, `refusal`→error, `pause_turn`→stop

**Usage:** `input_tokens`, `output_tokens`, `cache_read_input_tokens`, `cache_creation_input_tokens` (no total — computed)

**Thinking modes:**
- Adaptive (Opus 4.6+, Sonnet 4.6): effort levels
- Budget-based (older): token budget

**Cache control:** `cache_control: {type: "ephemeral", ttl?: "1h"}` on system, last tool, last user message

### Google Gemini / Vertex

**Content format:** `Content[]` with `role: "user"|"model"`, `parts: Part[]`

**Part types:**
- `{text}` — text content
- `{thought: true, text}` — thinking
- `{functionCall: {name, args, id?}}` — tool call
- `{functionResponse: {name, response, parts?, id?}}` — tool result
- `{inlineData: {mimeType, data}}` — images

**Thought signatures:** `thoughtSignature` field (base64, opaque) — context preservation, NOT a thinking indicator

**Stop reasons:** `STOP`, `MAX_TOKENS`, `SAFETY`, `BLOCKLIST`, `PROHIBITED_CONTENT`

### AWS Bedrock ConverseStream

**SDK-based:** `@aws-sdk/client-bedrock-runtime` `ConverseStreamCommand`

**Events:** `ContentBlockStartEvent`, `ContentBlockDeltaEvent`, `ContentBlockStopEvent`, `ConverseStreamMetadataEvent`

**Auth:** SigV4 (default) or Bearer token

**Thinking:** Budget-based with `thinkingBudgetTokens`

**Cache:** `CachePointType`, `CacheTTL`

---

## Caching Strategies

| Provider | Mechanism |
|----------|-----------|
| Anthropic | `cache_control: {type: "ephemeral"}` on system/last-tool/last-user |
| Anthropic (long) | `cache_control: {type: "ephemeral", ttl: "1h"}` |
| OpenAI Responses | `prompt_cache_key: sessionId`, `prompt_cache_retention: "24h"` |
| OpenAI Completions | `prompt_cache_key: sessionId` (api.openai.com only) |
| OpenRouter (Anthropic) | Anthropic-style `cache_control` markers |
| Bedrock (Claude) | `CachePoint` blocks after system/last-user |
| Mistral | `x-affinity: sessionId` header (KV-cache routing) |
| Google/Vertex | Automatic (`cachedContentTokenCount` in usage, read-only) |

### Session Affinity Headers
- `x-session-affinity: sessionId` — Fireworks, Cloudflare
- `session_id` header — OpenAI Responses

---

## Retry & Backoff

### Application-Level
- Exponential: `baseDelayMs * 2^(attempt-1)` (default base: 1000ms)
- Max retries: 3 (configurable)
- Abortable during sleep
- Context overflow NOT retried (handled by compaction)

### Retryable Error Patterns
```
overloaded|rate.?limit|429|500|502|503|504|service.?unavailable|
network.?error|connection.?refused|connection.?lost|fetch failed|
socket hang up|timed? out|timeout|terminated|retry delay
```

### Provider SDK-Level
- `maxRetries: 2` (OpenAI/Anthropic SDKs)
- `maxRetryDelayMs: 60000` — max server-requested delay before failing
- `timeoutMs: 600000` (10 min default)

---

## Context Overflow Detection

| Provider | Detection Method |
|----------|-----------------|
| Anthropic | `prompt is too long`, `request_too_large` (413) |
| Bedrock | `input is too long for requested model` |
| OpenAI | `exceeds the context window` |
| Google | `input token count.*exceeds the maximum` |
| xAI | `maximum prompt length is \d+` |
| Groq | `reduce the length of the messages` |
| OpenRouter | `maximum context length is \d+ tokens` |
| Mistral | `too large for model with \d+ maximum context length` |
| z.ai | Silent: `usage.input > contextWindow` on success |
| Xiaomi | `stopReason="length" && output=0 && input >= contextWindow*0.99` |

---

## Streaming Protocol

### StreamOptions (common)
```
temperature, maxTokens, signal, apiKey,
transport: "sse"|"websocket"|"websocket-cached"|"auto",
cacheRetention: "none"|"short"|"long",
sessionId, headers, timeoutMs, maxRetries, maxRetryDelayMs,
onPayload, onResponse, metadata
```

### SimpleStreamOptions (adds reasoning)
```
reasoning: "minimal"|"low"|"medium"|"high"|"xhigh",
thinkingBudgets: { minimal?, low?, medium?, high? }
```

### Normalized Event Stream
```
start → text_start → text_delta* → text_end →
thinking_start → thinking_delta* → thinking_end →
toolcall_start → toolcall_delta* → toolcall_end →
done | error
```

---

## Normalized Response Format

### AssistantMessage (universal output)
```
role: "assistant"
content: (TextContent | ThinkingContent | ToolCall)[]
model, responseModel, responseId
usage: { input, output, cacheRead, cacheWrite, totalTokens, cost }
stopReason: "stop"|"length"|"toolUse"|"error"|"aborted"
timestamp
```

### Content Block Types
- **TextContent:** `{type: "text", text, textSignature?}`
- **ThinkingContent:** `{type: "thinking", thinking, thinkingSignature?, redacted?}`
- **ToolCall:** `{type: "toolCall", id, name, arguments, thoughtSignature?}`

### Signatures (opaque, provider-specific)
- Anthropic thinking: encrypted base64 blob (multi-turn continuity)
- OpenAI Responses reasoning: serialized `ResponseReasoningItem` JSON
- OpenAI Completions reasoning: field name used (`"reasoning_content"`, `"reasoning"`)
- Google thought: base64 thought signature

---

## Cross-Provider Message Transformation

Before sending to any provider, `transformMessages()`:
1. Image downgrade for non-vision models
2. Thinking blocks: same model → keep; different model → convert to text; redacted → drop
3. Tool call ID normalization (Anthropic: 64 chars `[a-zA-Z0-9_-]`, Mistral: 9 chars, OpenAI: 40 chars)
4. Orphaned tool calls → synthetic error results
5. Error/aborted messages → skipped
6. Signatures stripped when crossing providers

---

## Environment Variables

| Provider | Key |
|----------|-----|
| OpenAI | `OPENAI_API_KEY` |
| Anthropic | `ANTHROPIC_API_KEY`, `ANTHROPIC_OAUTH_TOKEN` |
| Google | `GEMINI_API_KEY` |
| Vertex | `GOOGLE_CLOUD_API_KEY` (or ADC) |
| Bedrock | `AWS_PROFILE`, `AWS_ACCESS_KEY_ID`+`SECRET`, `AWS_BEARER_TOKEN_BEDROCK` |
| OpenRouter | `OPENROUTER_API_KEY` |
| DeepSeek | `DEEPSEEK_API_KEY` |
| xAI | `XAI_API_KEY` |
| Groq | `GROQ_API_KEY` |
| Cerebras | `CEREBRAS_API_KEY` |
| Mistral | `MISTRAL_API_KEY` |
| Fireworks | `FIREWORKS_API_KEY` |
| Together | `TOGETHER_API_KEY` |
| HuggingFace | `HF_TOKEN` |
| Cloudflare | `CLOUDFLARE_API_KEY` |

---

## CClaw Relevance

CClaw currently handles only OpenAI Completions format (via OpenRouter). Key gaps if going direct:
- Anthropic: completely different wire format (content blocks, not choices)
- Google: different format (Content/Parts)
- Bedrock: SDK-based

Since OpenRouter normalizes everything to OpenAI Completions, CClaw is covered for all providers through that gateway. Direct provider support would need separate parser modules per wire protocol family.
