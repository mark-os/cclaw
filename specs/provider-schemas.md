# Provider Schemas

Per-provider wire format for LLM API requests and responses (streaming and
non-streaming). Distilled from Pi's production-tested parsers.

**Pi source files** (in `reference/pi/packages/ai/src/providers/`):
- `openai-completions.ts` — OpenAI-compatible request building + streaming
- `google-shared.ts` — Gemini message conversion, tool conversion, stop reasons
- `google.ts` — Gemini streaming loop + thinking config
- `anthropic.ts` — Anthropic SSE parsing, message conversion, thinking modes

CClaw normalizes all providers into a single internal representation:
- **content** (text tokens)
- **reasoning** (thinking tokens)
- **tool_calls** (function invocations)
- **finish_reason** (stop, length, toolUse)
- **usage** (input, output, cache tokens)

---

## 1. OpenAI Chat Completions

Used by: OpenAI, DeepSeek, xAI, Groq, Cerebras, OpenRouter, Together,
Cloudflare, Fireworks, Mistral (via compat), and all OpenAI-compatible APIs.

### Request

```
POST /v1/chat/completions
Content-Type: application/json
Authorization: Bearer <key>
```

```json
{
  "model": "gpt-4o",
  "messages": [
    {"role": "system", "content": "..."},
    {"role": "user", "content": "..."},
    {"role": "assistant", "content": "...", "tool_calls": [...]},
    {"role": "tool", "tool_call_id": "call_abc", "content": "..."}
  ],
  "tools": [{"type": "function", "function": {"name": "...", "description": "...", "parameters": {...}}}],
  "max_completion_tokens": 4096,
  "stream": true,
  "stream_options": {"include_usage": true}
}
```

**Thinking configuration variants:**
| Provider | Format |
|----------|--------|
| OpenAI | `"reasoning_effort": "high"` top-level |
| DeepSeek | `"thinking": {"type": "enabled"}` + `"reasoning_effort"` |
| OpenRouter | `"reasoning": {"effort": "high"}` |
| Together | `"reasoning": {"enabled": true}` + optional `"reasoning_effort"` |
| z.ai / Qwen | `"enable_thinking": true` top-level |

**Cache control (Anthropic via OpenRouter):**
```json
{"type": "text", "text": "...", "cache_control": {"type": "ephemeral"}}
```
Applied to: last system content block, last tool definition, last user message.

### Non-Streaming Response

```json
{
  "id": "chatcmpl-abc123",
  "model": "gpt-4o",
  "choices": [{
    "index": 0,
    "message": {
      "role": "assistant",
      "content": "Hello world",
      "tool_calls": [{
        "id": "call_abc123",
        "type": "function",
        "function": {
          "name": "get_weather",
          "arguments": "{\"location\":\"NYC\"}"
        }
      }],
      "reasoning_content": "Let me think about this..."
    },
    "finish_reason": "stop"
  }],
  "usage": {
    "prompt_tokens": 42,
    "completion_tokens": 10,
    "total_tokens": 52,
    "prompt_tokens_details": {
      "cached_tokens": 20,
      "cache_write_tokens": 0
    },
    "completion_tokens_details": {
      "reasoning_tokens": 5
    }
  }
}
```

**Reasoning field variants (non-streaming `message.*`):**
- `reasoning_content` — DeepSeek, llama.cpp
- `reasoning` — OpenRouter, generic
- `reasoning_text` — some providers

### Streaming (SSE)

Response: `text/event-stream`. Each chunk: `data: <json>\n\n`.
Terminal: `data: [DONE]\n\n`.

```json
{
  "id": "chatcmpl-abc123",
  "model": "gpt-4o",
  "choices": [{
    "index": 0,
    "delta": {
      "content": "Hello",
      "tool_calls": [{
        "index": 0,
        "id": "call_abc123",
        "function": {"name": "get_weather", "arguments": "{\"loc"}
      }],
      "reasoning_content": "Let me think..."
    },
    "finish_reason": null
  }],
  "usage": { ... }
}
```

### Field Paths

| Data | Non-streaming | Streaming |
|------|---------------|-----------|
| Text | `choices[0].message.content` | `choices[0].delta.content` |
| Reasoning | `choices[0].message.reasoning_content` | `choices[0].delta.reasoning_content` |
| Tool calls | `choices[0].message.tool_calls[]` | `choices[0].delta.tool_calls[]` |
| Finish | `choices[0].finish_reason` | `choices[0].finish_reason` |
| Usage | `usage` | `usage` (final chunk only) |

### Finish Reason Mapping

| Wire | Internal |
|------|----------|
| `stop`, `end` | stop |
| `length` | length |
| `tool_calls`, `function_call` | toolUse |
| `content_filter` | error |

### Usage Calculation

```
input = prompt_tokens - cached_tokens - cache_write_tokens
output = completion_tokens  (includes reasoning_tokens)
cacheRead = prompt_tokens_details.cached_tokens OR prompt_cache_hit_tokens (DeepSeek)
cacheWrite = prompt_tokens_details.cache_write_tokens
```

### Quirks

- **Tool call assembly (streaming)**: Args arrive as string fragments across
  many chunks. Accumulate by `tool_calls[].index`. `id` and `name` appear once.
- **Reasoning priority**: Check `reasoning_content`, `reasoning`, `reasoning_text`
  in order; use first non-empty (some providers duplicate across fields).
- **DeepSeek**: Requires `reasoning_content: ""` on assistant messages in history.
- **`max_tokens` vs `max_completion_tokens`**: Older/compat providers use
  `max_tokens`; standard OpenAI uses `max_completion_tokens`.
- **Usage placement (streaming)**: Standard is top-level `chunk.usage` in final
  chunk. Moonshot puts it in `choices[0].usage`.
- **Empty deltas**: Chunks with `delta: {}` are valid. Don't crash.

### Caching & System Messages

- **Implicit prefix caching**: Automatic for all providers behind OpenRouter.
  Server detects identical prefix across requests, caches internally, reports
  savings via `prompt_tokens_details.cached_tokens`. No wire format changes.
- **128-token granularity** (OpenAI): Caches at every 128-token boundary
  starting at 1024 tokens. This means sub-prefixes of prior requests are
  cached — appending varying content at the end doesn't prevent cache hits
  on the stable portion at the start.
- **Growing conversations are inherently cache-friendly**: Each turn appends
  to the end of the messages array; the prefix (all prior turns) is stable and
  gets cache reads automatically. Only the new tail incurs full input cost.
- **What breaks the cache**: Changing `tools` or `system` — these are
  serialized first in the prefix (`tools → system → messages`). Any byte
  change there invalidates the entire cache for that request. Tool changes
  mid-session are a one-turn cache miss.
- **Multiple system messages**: Supported anywhere in the messages array.
  All are treated as system-level instructions regardless of position.
- **Cache-safe injection**: Append new system/developer messages at the *end*
  of the messages array (after all prior turns). The cached prefix is preserved.
- **DeepSeek**: Follows OpenAI prefix rules. Reports `prompt_cache_hit_tokens`.
- **OpenRouter sticky routing**: Routes subsequent requests in the same
  conversation to the same physical backend. Use `session_id` for explicit
  control over sticky sessions.

---

## 2. Google Gemini

Used by: Google Generative AI API (direct), Google Vertex AI.

### Request

```
POST /v1beta/models/{model}:generateContent
Content-Type: application/json
x-goog-api-key: <key>
```

Streaming variant: `:streamGenerateContent?alt=sse`

```json
{
  "systemInstruction": {"parts": [{"text": "..."}]},
  "contents": [
    {"role": "user", "parts": [{"text": "..."}, {"inlineData": {"mimeType": "image/png", "data": "base64"}}]},
    {"role": "model", "parts": [
      {"text": "thinking...", "thought": true, "thoughtSignature": "base64..."},
      {"text": "response"},
      {"functionCall": {"name": "get_weather", "args": {"loc": "NYC"}, "id": "call_1"}}
    ]},
    {"role": "user", "parts": [{"functionResponse": {"name": "get_weather", "response": {"output": "72F"}, "id": "call_1"}}]}
  ],
  "tools": [{"functionDeclarations": [{"name": "...", "description": "...", "parameters": {...}}]}],
  "generationConfig": {"maxOutputTokens": 4096},
  "thinkingConfig": {"includeThoughts": true, "thinkingBudget": 8192}
}
```

**Thinking configuration:**
- Gemini 2.5: `"thinkingConfig": {"thinkingBudget": N}` (0 to disable, -1 for dynamic)
- Gemini 3.x: `"thinkingConfig": {"thinkingLevel": "MINIMAL"|"LOW"|"MEDIUM"|"HIGH"}`

### Non-Streaming Response

```json
{
  "candidates": [{
    "content": {
      "parts": [
        {"text": "thinking...", "thought": true},
        {"text": "Hello world", "thoughtSignature": "base64..."},
        {"functionCall": {"name": "get_weather", "args": {"loc": "NYC"}}}
      ],
      "role": "model"
    },
    "finishReason": "STOP",
    "index": 0
  }],
  "usageMetadata": {
    "promptTokenCount": 42,
    "candidatesTokenCount": 10,
    "totalTokenCount": 52,
    "cachedContentTokenCount": 20,
    "thoughtsTokenCount": 100
  },
  "modelVersion": "gemini-2.5-flash",
  "responseId": "abc123"
}
```

### Streaming (SSE)

Response: `text/event-stream`. Each chunk: `data: <json>\n\n`.
No `[DONE]` marker — stream ends when HTTP response completes.

Same JSON shape as non-streaming, but `parts[]` contains incremental fragments:
```json
{"candidates": [{"content": {"parts": [{"text": "Hello"}], "role": "model"}, "index": 0}], "usageMetadata": {...}}
```

### Field Paths

| Data | Path | Notes |
|------|------|-------|
| Text | `candidates[0].content.parts[].text` | When `thought` absent/false |
| Thinking | `candidates[0].content.parts[].text` | When `thought: true` |
| Thought signature | `parts[].thoughtSignature` | On ANY part type |
| Tool call | `parts[].functionCall` | `{name, args, id?}` |
| Finish reason | `candidates[0].finishReason` | UPPER_CASE |
| Usage | `usageMetadata` | Present on every streaming chunk |

### Part Type Detection

Single `parts[]` array carries all content types:
- `thought: true` → thinking
- `functionCall` present → tool call
- Otherwise → text content
- `thoughtSignature` does NOT indicate thinking (context preservation only)

### Finish Reason Mapping

| Wire | Internal |
|------|----------|
| `STOP` | stop |
| `MAX_TOKENS` | length |
| `SAFETY`, `BLOCKLIST`, `PROHIBITED_CONTENT`, `RECITATION` | error |
| `MALFORMED_FUNCTION_CALL` | error |

Note: When tool calls are present, `finishReason` is still `STOP`. Detect
tool use by presence of `functionCall` parts.

### Usage Calculation

```
input = promptTokenCount - cachedContentTokenCount
output = candidatesTokenCount + thoughtsTokenCount
cacheRead = cachedContentTokenCount
cacheWrite = 0  (automatic caching, no write metric)
```

### Quirks

- **Complete tool args**: Unlike OpenAI, `functionCall.args` is a complete JSON
  object in one shot. No incremental assembly needed.
- **No `[DONE]`**: Stream ends at EOF.
- **Tool call IDs optional**: Generate synthetic IDs when missing.
- **Multiple parts per chunk**: Iterate all parts; a chunk can have text +
  functionCall in the same response.
- **usageMetadata on every chunk**: Cumulative. Use the last values.
- **thoughtSignature**: Opaque base64. Must be replayed verbatim on corresponding
  part in subsequent turns. Only valid for same provider+model. Validate: base64
  format, length % 4 == 0.

### Caching & System Messages

- **Implicit prefix caching** (Gemini 2.5+): Automatic. Server detects identical
  prefix, charges reduced rate (`cachedContentTokenCount` in usageMetadata).
  Same behavior as OpenAI — send the same contents, get savings. Min 2048–4096
  tokens depending on model. No wire format changes needed.
- **Explicit `cachedContents` API** (not used by CClaw): Separate API to create
  named cache objects with hourly storage fees. Designed for fan-out use cases
  (many users sharing one large static prefix like a video or document).
  Not useful for single-user growing conversations where implicit caching
  already handles the prefix.
- **`systemInstruction` is top-level only**: Cannot place system messages in
  `contents[]`. Changing `systemInstruction` invalidates any cached prefix.
- **No mid-conversation system role**: Unlike OpenAI/Anthropic, Gemini has no
  `role: "system"` in the contents array. System-level instructions after session
  start must go as user messages.
- **Implication for CClaw**: System entries added mid-session should be emitted
  as user messages in the Gemini wire format (not aggregated into systemInstruction)
  to preserve the cached prefix. Only the *initial* system prompt goes in
  systemInstruction. Recall text should also go as a trailing user message
  for Gemini (not in systemInstruction) to avoid cache-busting.

---

## 3. Anthropic Messages

Used by: Anthropic direct, AWS Bedrock (via proxy), Fireworks (Anthropic-compat).

### Request

```
POST /v1/messages
Content-Type: application/json
x-api-key: <key>
anthropic-beta: interleaved-thinking-2025-05-14
```

```json
{
  "model": "claude-sonnet-4-20250514",
  "system": [{"type": "text", "text": "...", "cache_control": {"type": "ephemeral"}}],
  "messages": [
    {"role": "user", "content": "..."},
    {"role": "assistant", "content": [
      {"type": "thinking", "thinking": "...", "signature": "base64..."},
      {"type": "text", "text": "..."},
      {"type": "tool_use", "id": "toolu_abc", "name": "get_weather", "input": {"loc": "NYC"}}
    ]},
    {"role": "user", "content": [
      {"type": "tool_result", "tool_use_id": "toolu_abc", "content": "72F"}
    ]}
  ],
  "tools": [{"name": "...", "description": "...", "input_schema": {"type": "object", ...}}],
  "max_tokens": 4096,
  "stream": true,
  "thinking": {"type": "enabled", "budget_tokens": 8192}
}
```

**Thinking configuration:**
- Budget-based (older models): `"thinking": {"type": "enabled", "budget_tokens": N}`
- Adaptive (Opus 4.6+, Sonnet 4.6): `"thinking": {"type": "adaptive", "display": "summarized"}`
  + `"output_config": {"effort": "high"}`
- Disabled: `"thinking": {"type": "disabled"}`

**Cache control**: `"cache_control": {"type": "ephemeral", "ttl": "1h"}` on
system blocks, last tool definition, last user message content block.

### Non-Streaming Response

```json
{
  "id": "msg_abc123",
  "model": "claude-sonnet-4-20250514",
  "content": [
    {"type": "thinking", "thinking": "Let me think...", "signature": "base64..."},
    {"type": "text", "text": "Hello world"},
    {"type": "tool_use", "id": "toolu_abc", "name": "get_weather", "input": {"loc": "NYC"}}
  ],
  "stop_reason": "end_turn",
  "usage": {
    "input_tokens": 42,
    "output_tokens": 15,
    "cache_read_input_tokens": 20,
    "cache_creation_input_tokens": 5
  }
}
```

### Streaming (SSE)

Response: `text/event-stream` with **typed events**.
Format: `event: <type>\ndata: <json>\n\n`.

**Event sequence:**
```
message_start → content_block_start → content_block_delta* →
content_block_stop → [repeat blocks] → message_delta → message_stop
```

**Event shapes:**

```json
// message_start
{"type": "message_start", "message": {"id": "msg_abc", "usage": {"input_tokens": 42, ...}}}

// content_block_start
{"type": "content_block_start", "index": 0, "content_block": {"type": "text"}}
{"type": "content_block_start", "index": 1, "content_block": {"type": "thinking"}}
{"type": "content_block_start", "index": 2, "content_block": {"type": "redacted_thinking", "data": "base64"}}
{"type": "content_block_start", "index": 3, "content_block": {"type": "tool_use", "id": "toolu_abc", "name": "get_weather"}}

// content_block_delta
{"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": "Hello"}}
{"type": "content_block_delta", "index": 1, "delta": {"type": "thinking_delta", "thinking": "Let me..."}}
{"type": "content_block_delta", "index": 3, "delta": {"type": "input_json_delta", "partial_json": "{\"loc"}}
{"type": "content_block_delta", "index": 1, "delta": {"type": "signature_delta", "signature": "chunk..."}}

// content_block_stop
{"type": "content_block_stop", "index": 0}

// message_delta
{"type": "message_delta", "delta": {"stop_reason": "end_turn"}, "usage": {"output_tokens": 15, ...}}

// message_stop
{"type": "message_stop"}
```

### Field Paths (Non-Streaming)

| Data | Path |
|------|------|
| Text | `content[].text` (where `type == "text"`) |
| Thinking | `content[].thinking` (where `type == "thinking"`) |
| Thinking signature | `content[].signature` |
| Tool call ID | `content[].id` (where `type == "tool_use"`) |
| Tool call name | `content[].name` |
| Tool call args | `content[].input` |
| Stop reason | `stop_reason` |
| Usage | `usage` |

### Finish Reason Mapping

| Wire | Internal |
|------|----------|
| `end_turn` | stop |
| `max_tokens` | length |
| `tool_use` | toolUse |
| `refusal` | error |
| `pause_turn` | stop |

### Usage Calculation

```
input = input_tokens
output = output_tokens
cacheRead = cache_read_input_tokens
cacheWrite = cache_creation_input_tokens
total = input + output + cacheRead + cacheWrite  (computed)
```

### Quirks

- **Typed events**: Must parse `event:` line before `data:` line. Event type
  determines how to interpret the JSON payload.
- **Index-based blocks**: Each block gets an `index` at `content_block_start`.
  Deltas reference the same index. Maintain index → block state map.
- **Interleaved thinking**: Thinking blocks can appear between text/tool blocks
  (not just at start). Multiple thinking blocks possible.
- **Tool args as JSON fragments**: `partial_json` accumulates across deltas.
  Need streaming JSON parser for early access.
- **Signature accumulation**: `signature_delta` events append to thinking
  block's signature. Complete only at `content_block_stop`.
- **Redacted thinking**: Entire blob at `content_block_start`. No deltas follow.
  Replay as `{"type": "redacted_thinking", "data": "..."}`.
- **Usage split**: Input tokens in `message_start`, output in `message_delta`.
  Some proxies omit input in `message_delta` — keep `message_start` values.
- **No `[DONE]`**: Ends with `message_stop` event.
- **Tool call ID format**: 64 chars max, `[a-zA-Z0-9_-]` only.
- **Error events**: `event: error` can arrive anytime. Abort.

### Caching & System Messages

- **Opt-in prefix caching**: Requires `"cache_control": {"type": "ephemeral"}`
  to activate. Two modes:
  - **Automatic** (recommended for conversations): Single top-level
    `"cache_control": {"type": "ephemeral"}` on the request body. Server
    auto-places the breakpoint on the last cacheable block and advances it
    each turn. Growing conversation prefix gets cache reads (0.1× base cost),
    new tail gets a cache write (1.25× base cost). Net effect: sub-linear
    cost growth per turn.
  - **Explicit breakpoints**: Place `cache_control` on up to 4 individual
    content blocks for fine-grained control over what's cached separately.
- **Still a single request format**: Unlike Gemini's `cachedContents` API,
  Anthropic caching does NOT require a separate API call. You send the full
  messages every turn — the server handles caching internally based on the
  breakpoint markers. No 2-step create-then-reference flow.
- **5-minute TTL** (default), refreshed on each cache hit. 1-hour TTL
  available at 2× base cost for infrequent requests.
- **Top-level `system` field is cached prefix**: Changing it invalidates
  the entire cache for everything that follows.
- **Mid-conversation `{"role": "system"}` messages**: Supported in the messages
  array (Claude Opus 4.8+). These are appended after the cached prefix and
  treated as operator-level instructions without breaking the cache.
- **Cache hash order**: tools → system → messages (prefix-based, like OpenAI).
- **Implication for CClaw**: Add top-level `"cache_control": {"type": "ephemeral"}`
  to Anthropic requests. The payload builder already produces a deterministic
  prefix; this one field activates the cost savings. No per-block markers needed.

---

## 4. CClaw Parser Design

### Current State
Single `sse_process_line()` with auto-detection (strstr-based, fragile).

### Proposed Refactor
Per-provider parsers using jsmn token traversal:

```c
typedef struct {
    const char *text;       size_t text_len;
    const char *reasoning;  size_t reasoning_len;
    int is_thinking;
    const char *tc_name;    size_t tc_name_len;
    const char *tc_id;      size_t tc_id_len;
    const char *tc_args;    size_t tc_args_len;
    int tc_args_complete;   /* 1 for Gemini (full obj), 0 for OpenAI/Anthropic (fragment) */
    const char *finish;     size_t finish_len;
    int prompt_tokens;
    int completion_tokens;
    int cache_read_tokens;
} SseChunk;

int sse_parse_openai(const char *json, size_t len, jsmntok_t *tok, int ntok, SseChunk *out);
int sse_parse_gemini(const char *json, size_t len, jsmntok_t *tok, int ntok, SseChunk *out);
int sse_parse_anthropic(const char *event_type, const char *json, size_t len,
                        jsmntok_t *tok, int ntok, SseChunk *out);
```

### Anthropic Framing

Requires event-type capture (`event:` line). Extend SSE line splitter to:
1. Track current event type
2. On `data:` line, pass event type + data to parser

### Non-Streaming Parsers

Separate functions for full response parsing (no accumulation needed):

```c
int parse_response_openai(Arena *a, const char *json, size_t len, LlmResponse *out);
int parse_response_gemini(Arena *a, const char *json, size_t len, LlmResponse *out);
int parse_response_anthropic(Arena *a, const char *json, size_t len, LlmResponse *out);
```

### jsmn Token Budget
- OpenAI chunks: ~64 tokens sufficient
- Gemini chunks (multi-part): ~256 tokens
- Anthropic events: ~64 tokens
- Non-streaming full responses: dynamic (realloc if needed)
