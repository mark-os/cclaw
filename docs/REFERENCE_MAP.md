# CClaw Reference Map

A detailed map of the most important files in [Pi](https://github.com/earendil-works/pi) and [OpenClaw](https://github.com/openclaw/openclaw) relevant to CClaw's C reimplementation. These are battle-tested codebases with comprehensive error handling and edge case coverage.

**CClaw's minimal scope:** Telegram bot → agent loop → OpenAI-compatible LLM endpoints.

## Table of Contents

1. [Pi AI Package (`pi-ai`)](#pi-ai-package-pi-ai) — LLM client, streaming, provider abstraction
2. [Pi Agent Core (`pi-agent-core`)](#pi-agent-core-pi-agent-core) — Agent loop, tool execution, events
3. [OpenClaw Integration](#openclaw-integration) — Pi embedding, Telegram channel, message flow

---

## Pi AI Package (`pi-ai`)

Path: `reference/pi/packages/ai/`

The `pi-ai` package provides a unified streaming interface for calling LLM providers. It defines a common type system for messages, tools, and usage; a provider registry that dispatches by API identifier; and an event-stream protocol that delivers incremental content blocks (text, thinking, tool calls) as async iterables. The OpenAI Completions provider is the most relevant for CClaw.

### Key Types

**Source:** `src/types.ts`

Core enums/unions:
- `Api` — `"openai-completions"` | `"anthropic-messages"` | `"openai-responses"` | `"google-generative-ai"` | etc.
- `Provider` — `"openai"` | `"anthropic"` | `"deepseek"` | `"groq"` | etc.
- `ThinkingLevel` — `"minimal"` | `"low"` | `"medium"` | `"high"` | `"xhigh"`
- `CacheRetention` — `"none"` | `"short"` | `"long"`
- `Transport` — `"sse"` | `"websocket"` | `"websocket-cached"` | `"auto"`
- `StopReason` — `"stop"` | `"length"` | `"toolUse"` | `"error"` | `"aborted"`

Message types:
```
UserMessage { role: "user", content: string | (TextContent | ImageContent)[], timestamp }
AssistantMessage { role: "assistant", content: (TextContent | ThinkingContent | ToolCall)[], api, provider, model, responseId?, usage, stopReason, errorMessage?, timestamp }
ToolResultMessage { role: "toolResult", toolCallId, toolName, content: (TextContent | ImageContent)[], details?, isError, timestamp }
```

Content block types:
- `TextContent { type: "text", text }`
- `ThinkingContent { type: "thinking", thinking, redacted? }`
- `ImageContent { type: "image", data (base64), mimeType }`
- `ToolCall { type: "toolCall", id, name, arguments: Record<string,any> }`

Other key types:
```
Model<TApi> { id, name, api, provider, baseUrl, reasoning, contextWindow, maxTokens, cost, headers? }
Tool { name, description, parameters: JSON Schema }
Context { systemPrompt?, messages: Message[], tools?: Tool[] }
Usage { input, output, cacheRead, cacheWrite, totalTokens, cost }
StreamOptions { temperature?, maxTokens?, signal?, apiKey?, transport?, cacheRetention?, timeoutMs? }
```

Event protocol (`AssistantMessageEvent`):
```
start | text_start | text_delta | text_end | thinking_start | thinking_delta | thinking_end
toolcall_start | toolcall_delta | toolcall_end | done | error
```

### Streaming & Provider Architecture

**Source:** `src/stream.ts`, `src/api-registry.ts`, `src/utils/event-stream.ts`

Entry points:
- `streamSimple(model, context, options?)` → `AssistantMessageEventStream` — the main LLM call
- `complete(model, context, options?)` → `Promise<AssistantMessage>` — awaits stream result
- All resolve provider via `getApiProvider(model.api)` from the registry

Provider registry (`api-registry.ts`):
- `registerApiProvider({ api, stream, streamSimple }, sourceId?)` — registers at import time
- `getApiProvider(api)` → provider or undefined
- Providers registered via side-effect import in `stream.ts`

EventStream (`utils/event-stream.ts`):
- `EventStream<T, R>` — push-based async iterable with final result promise
- `push(event)` / `end(result?)` / `[Symbol.asyncIterator]()` / `result()`
- **Contract:** Once a StreamFunction is invoked, all errors must be encoded in the stream as an `"error"` event. Providers must NOT throw.

### OpenAI Completions Provider

**Source:** `src/providers/openai-completions.ts` (~600 lines — the one CClaw will reimplement)

Architecture:
1. Creates OpenAI client with apiKey + baseURL
2. Builds request params via `buildParams()` mapping Context → ChatCompletionCreateParams
3. Calls `client.chat.completions.create(params).withResponse()`
4. Iterates SSE stream chunks, emits events to EventStream
5. On any error → emit `"error"` event, call `stream.end()`

Request building (`buildParams`):
- System prompt → `system` role (or `developer` for reasoning models)
- User messages → text or content array with `image_url` blocks
- Assistant messages → text + `tool_calls` array
- Tool results → `tool` role with `tool_call_id`
- Tools → `{ type: "function", function: { name, description, parameters, strict } }`
- Adds `stream: true`, `stream_options: { include_usage: true }`

SSE stream parsing:
- Each chunk has `choices[0].delta` with: `content`, `reasoning_content`, `tool_calls[]`
- Tool calls arrive interleaved by `index` — accumulate per-index buffers
- Tool call arguments are string fragments, parsed on completion
- `finish_reason` maps: `"stop"` → stop, `"tool_calls"` → toolUse, `"length"` → length, `"content_filter"` → error
- Usage from `chunk.usage`: `input = prompt_tokens - cached - cache_write`

Message transformation (`transform-messages.ts`):
- Strips errored/aborted assistant messages from history
- Inserts synthetic "No result provided" for orphaned tool calls
- Normalizes tool call IDs (truncate to 40 chars)
- Downgrades images to text for non-vision models

### Error Handling Patterns

1. **Stream-level encapsulation:** All errors caught → `stopReason: "error"`, `errorMessage`, emit error event
2. **Abort:** Check `signal.aborted` → `stopReason: "aborted"`
3. **Missing finish_reason:** Stream ends without one → error
4. **Context overflow** (`utils/overflow.ts`): Pattern-matches errorMessage against 20+ regex patterns for all providers
5. **Orphaned tool calls:** Insert synthetic error results before sending history
6. **Empty tools:** Never send `"tools": []` — omit field entirely

### Test Catalog

| File | What it tests | Key scenarios |
|------|--------------|---------------|
| `test/stream.test.ts` | Core streaming across providers | Text gen, multi-turn, tool calls, image input, streaming events |
| `test/abort.test.ts` | Abort/cancellation | Mid-stream abort, immediate abort, abort-then-continue |
| `test/context-overflow.test.ts` | Context window overflow | Exceeding contextWindow, `isContextOverflow()` detection |
| `test/openai-completions-tool-choice.test.ts` | Tool calling protocol | `toolChoice` forwarded: auto, required, specific function |
| `test/openai-completions-empty-tools.test.ts` | Empty tools edge case | `tools: []` must NOT be serialized (prevents 400 errors) |
| `test/empty.test.ts` | Empty/whitespace input | Empty content, whitespace-only — graceful handling |
| `test/total-tokens.test.ts` | Token counting & caching | `totalTokens` field, cache hit on second request |
| `test/tool-call-id-normalization.test.ts` | Cross-provider ID handling | Pipe-separated IDs normalized, live handoff tests |
| `test/tool-call-without-result.test.ts` | Missing tool results | Synthetic result inserted, second request succeeds |

### Patterns Relevant to C Implementation

- **SSE parsing is line-based:** `data: {...}\n\n` — parse `choices[0].delta`. Handle `[DONE]` sentinel.
- **Tool call index tracking:** Multiple parallel tool calls interleaved by index. Maintain buffer array keyed by index.
- **Incremental JSON for tool args:** Accumulate string fragments, parse only on `finish_reason: "tool_calls"`.
- **Error-as-value:** Never crash on provider errors. Always produce valid AssistantMessage with stopReason=error.
- **Content is always array of blocks:** Design C struct as dynamic array of tagged-union content blocks.
- **Strip errored messages from replay:** Skip AssistantMessages with stopReason error/aborted when building request.
- **Abort = close HTTP connection:** Then set stopReason = "aborted".
- **finish_reason is mandatory:** If stream ends without one, treat as error.
- **Usage calculation:** `input = prompt_tokens - cached_tokens - cache_write_tokens`.

---

## Pi Agent Core (`pi-agent-core`)

Path: `reference/pi/packages/agent/`

Implements a streaming agent loop that drives LLM conversations with tool execution. Three layers: a low-level functional loop (`agentLoop`/`agentLoopContinue`), a stateful `Agent` class with message queuing, and a higher-level `AgentHarness` with session persistence, compaction, and branching.

### Agent Loop Architecture

**Source:** `src/agent-loop.ts`

The loop has an **outer loop** (follow-up messages) and an **inner loop** (tool execution + steering):

```
agentLoop(prompts, context, config, signal, streamFn)
  → emit agent_start, turn_start, message_start/end for prompts
  → runLoop():

    pendingMessages = getSteeringMessages()

    OUTER LOOP (while true):
      hasMoreToolCalls = true

      INNER LOOP (while hasMoreToolCalls || pendingMessages.length > 0):
        1. emit turn_start (except first)
        2. Inject pendingMessages → emit message_start/end, push to context
        3. streamAssistantResponse() → AssistantMessage
           - If error/aborted: emit turn_end, agent_end, return
        4. Extract toolCalls from message content
        5. If toolCalls: executeToolCalls() → { messages, terminate }
           - Push tool results to context
           - hasMoreToolCalls = !terminate
        6. emit turn_end
        7. prepareNextTurn() → optionally swap context/model/thinkingLevel
        8. shouldStopAfterTurn() → if true, emit agent_end, return
        9. pendingMessages = getSteeringMessages()

      // Inner loop done — agent would stop
      followUpMessages = getFollowUpMessages()
      If exist: set as pendingMessages, continue outer loop
      Else: break

    emit agent_end
```

**streamAssistantResponse():**
1. Apply `transformContext` (AgentMessage[] → AgentMessage[])
2. Apply `convertToLlm` (AgentMessage[] → Message[] for LLM)
3. Build LLM context with systemPrompt + tools
4. Resolve API key via `getApiKey`
5. Call `streamFn` (defaults to `streamSimple`)
6. Emit message_start, message_update (deltas), message_end

### Key Types

**Source:** `src/types.ts`

| Type | Purpose |
|------|---------|
| `StreamFn` | `(model, context, options) → EventStream`. Must not throw. |
| `ToolExecutionMode` | `"sequential" \| "parallel"` |
| `QueueMode` | `"all" \| "one-at-a-time"` — queue drain behavior |
| `AgentToolCall` | `{ type: "toolCall", id, name, arguments }` from assistant |
| `AgentToolResult<T>` | `{ content, details, terminate? }` |
| `AgentTool` | Extends Tool with `label`, `execute()`, optional `prepareArguments()` |
| `AgentContext` | `{ systemPrompt, messages: AgentMessage[], tools?: AgentTool[] }` |
| `AgentMessage` | Union of Message types + custom via declaration merging |
| `AgentLoopConfig` | model, convertToLlm, transformContext, getApiKey, shouldStopAfterTurn, prepareNextTurn, getSteeringMessages, getFollowUpMessages, toolExecution, beforeToolCall, afterToolCall |
| `BeforeToolCallResult` | `{ block?, reason? }` — return `{ block: true }` to prevent execution |
| `AfterToolCallResult` | `{ content?, details?, isError?, terminate? }` — partial override |
| `AgentLoopTurnUpdate` | `{ context?, model?, thinkingLevel? }` — replacement state for next turn |

### Tool Execution Lifecycle

```
For each toolCall in assistantMessage.content:
  1. emit tool_execution_start { toolCallId, toolName, args }
  2. prepareToolCall():
     - Find tool by name → if not found, immediate error result
     - Call tool.prepareArguments(rawArgs) if defined
     - validateToolArguments(tool, args) via schema
     - Call config.beforeToolCall() → if { block: true }, error result
  3. executePreparedToolCall():
     - Call tool.execute(toolCallId, args, signal, onUpdate)
     - onUpdate → emit tool_execution_update
     - On throw → error result
  4. finalizeExecutedToolCall():
     - Call config.afterToolCall() → merge overrides
  5. emit tool_execution_end { toolCallId, toolName, result, isError }
  6. Create ToolResultMessage → emit message_start, message_end
```

**Parallel vs Sequential:**
- Sequential: tools execute one-by-one in order
- Parallel (default): prepare sequentially, execute concurrently, emit end in completion order, result messages in source order

**Batch termination:** Only when ALL tool results have `terminate === true`.

### Event Protocol

| Event | When | Payload |
|-------|------|---------|
| `agent_start` | Run begins | — |
| `agent_end` | Run complete | `{ messages }` |
| `turn_start` | Before each assistant response | — |
| `turn_end` | After response + tool results | `{ message, toolResults }` |
| `message_start` | Message begins | `{ message }` |
| `message_update` | Streaming delta | `{ message, assistantMessageEvent }` |
| `message_end` | Message finalized | `{ message }` |
| `tool_execution_start` | Tool begins | `{ toolCallId, toolName, args }` |
| `tool_execution_update` | Tool streams partial | `{ toolCallId, toolName, partialResult }` |
| `tool_execution_end` | Tool finished | `{ toolCallId, toolName, result, isError }` |

### Agent Class

**Source:** `src/agent.ts`

Stateful wrapper around the functional loop:
- Owns mutable state (messages, tools, isStreaming, errorMessage)
- Message queues: steering (mid-run injection) and follow-up (post-stop injection)
- API: `prompt(input)`, `continue()`, `steer(message)`, `followUp(message)`, `abort()`, `reset()`
- `subscribe(listener)` → unsubscribe function. Listeners awaited in order.
- Run lifecycle: creates AbortController, runs loop, synthesizes error events on failure

### Proxy/Middleware

**Source:** `src/proxy.ts`

`streamProxy()` — routes LLM calls through a proxy server:
- POST to `{proxyUrl}/api/stream` with model + context
- Server sends SSE with same event types
- Client reconstructs partial AssistantMessage locally
- Handles abort via signal → cancels reader

### Test Catalog

| File | What it tests | Key scenarios |
|------|--------------|---------------|
| `test/agent-loop.test.ts` | Low-level loop | Custom message types via convertToLlm; transformContext applied; tool calls/results; beforeToolCall args mutation; parallel execution order; steering message injection; batch termination; prepareNextTurn; shouldStopAfterTurn |
| `test/agent.test.ts` | Agent class | State creation; subscribe/unsubscribe; lifecycle events on failure; async subscriber awaiting; steering queue; follow-up queue; abort; throws when streaming; continue() semantics |
| `test/e2e.test.ts` | Integration with faux provider | Text prompt; tool execution with pending tracking; abort during streaming; multi-turn context; thinking blocks; continue() validation |

### Patterns Relevant to C Implementation

- **Event-driven:** Loop communicates via `emit(event)` callback → C function pointer `void (*emit)(AgentEvent*, void* userdata)`
- **Inner/outer loop:** Two nested `while` loops with a message queue (linked list)
- **Tool pipeline:** prepare → validate → beforeHook → execute → afterHook → finalize. Each stage can short-circuit.
- **Batch termination:** AND-reduce all `terminate` flags in batch
- **AbortSignal → cancellation token:** Shared `volatile int aborted` flag or pipe/eventfd
- **convertToLlm as filter:** Callback that transforms AgentMessage[] → LLM Message[] before each call
- **Parallel tools:** Thread pool or pthread_create per tool, join all, emit in completion order
- **No exceptions:** Tools must not crash the loop. Check return codes, errors become AgentToolResult with isError=true
- **Stateful wrapper separation:** Keep functional loop separate from stateful Agent (owns transcript, queues, abort)

---

## OpenClaw Integration

Path: `reference/openclaw/`

OpenClaw embeds the Pi coding agent and exposes it through multiple channels including Telegram. The architecture: a **runner** manages the agent loop, a **channel plugin** handles transport, and a **dispatch layer** bridges inbound messages to agent sessions.

### Pi Embedding Architecture

**Source:** `src/agents/pi-embedded-runner/run.ts`

Entry point: `runEmbeddedPiAgent(params) → Promise<EmbeddedPiRunResult>`

Flow:
1. Session key resolution (backfill from sessionId)
2. Lane queuing — per-session serialization + global rate limiting
3. Model resolution via `resolveModelAsync()`
4. Attempt loop with retry:
   - Failover on rate limit/billing/overload
   - Auth profile rotation across API keys
   - Compaction on context overflow
   - Idle timeout detection
5. Result assembly (texts, tool metas, usage)

Key params:
- `sessionId`, `sessionKey`, `sessionFile` — session identity + transcript path
- `prompt` — user message text
- `provider`, `model` — LLM target
- `timeoutMs`, `abortSignal` — timeout + cancellation
- `onBlockReply`, `onPartialReply` — streaming callbacks

Session lifecycle:
- Sessions identified by key: `agent:{agentId}:telegram:{dm|group}:{peerId}`
- Transcript persisted to JSONL file
- Creates Pi `AgentSession` via `createAgentSession()`
- Subscribes to stream via `subscribeEmbeddedPiSession()`
- Compaction summarizes old turns on context overflow

### System Prompt Construction

**Source:** `src/agents/pi-embedded-runner/system-prompt.ts`

Assembled sections (full mode):
1. Identity/persona
2. Tooling — ordered list of available tools with summaries
3. Skills — available skills block
4. Memory guidance
5. Workspace/runtime info (OS, host, model, channel)
6. Project context (AGENTS.md, SOUL.md, etc.)
7. Time/timezone

For CClaw minimal: static string with identity + tool descriptions + workspace path.

### Model Resolution

**Source:** `src/agents/pi-embedded-runner/model.ts`

For CClaw minimal: read API key from env, hardcode provider URL, send model name in request body. No discovery needed.

Full OpenClaw flow (for reference):
1. Config lookup (`models.json`)
2. Pi discovery (`discoverModels()`)
3. Plugin dynamic models
4. Inline `provider:model` syntax
5. Static catalog (context windows, capabilities)
6. Auth attachment (env vars, config, profiles)

### Telegram Channel Architecture

**Source:** `extensions/telegram/src/`

**channel.ts** (~40KB) — Plugin registration:
- Account resolution (multi-bot)
- Outbound message adapter
- Session routing (DM vs group, topic threads)
- Status/probe/monitoring

**bot-core.ts** (~15KB) — `createTelegramBotCore()`:
- Creates grammY Bot instance
- API throttling, per-chat sequential middleware
- Update tracking (dedup, watermark persistence)
- Typing indicator coalescing (4s)
- Error catching middleware

**polling-session.ts** (~29KB) — `TelegramPollingSession`:
- Long-polling via `@grammyjs/runner`
- Restart with exponential backoff (2s initial, 30s max, 1.8x factor, 0.25 jitter)
- Stall detection watchdog (120s threshold, 30s check interval)
- Graceful stop with 15s grace period
- Webhook clearing on first poll start
- Update offset persistence

**bot-message-dispatch.ts** (~70KB) — Core dispatch:
- Receives TelegramMessageContext from handlers
- Resolves session file + session key
- Builds agent prompt from message body
- Calls `runEmbeddedPiAgent()`
- Handles streaming (draft stream for live typing updates)
- Delivers replies (chunked markdown → Telegram HTML)
- Manages ack reactions, typing indicators, error policies

### Message Flow

```
Telegram Update (getUpdates long-poll)
  → grammY Bot middleware chain
    → Update tracker (dedup, watermark)
    → Per-chat sequential middleware
    → Handler dispatch
      → bot-message-context.ts (build context)
        → Ingress authorization check
        → bot-message-dispatch.ts (processMessage)
          → Resolve session key
          → Build prompt (strip mention, handle media)
          → runEmbeddedPiAgent()
            → LLM stream → Tool execution loop
            → onBlockReply callbacks
          → Final reply delivery (chunked, formatted)
          → Session transcript persistence
```

### Error Handling & Resilience

- **Model failover** — rate limit/billing/overload → rotate auth profile or fallback model
- **Compaction** — context overflow → summarize history, retry
- **Idle timeout breaker** — stuck LLM calls detected, retry with backoff
- **Telegram network errors** — classified as transient; polling auto-restarts with backoff
- **Update deduplication** — prevents processing same update twice
- **Typing indicator circuit breaker** — stops after consecutive 401s
- **Graceful shutdown** — drains pending deliveries, stops with grace period
- **Update offset persistence** — single integer in file, survives restart

### Test Catalog

| File | What it tests | Key scenarios |
|------|--------------|---------------|
| `extensions/telegram/src/bot.test.ts` (119KB) | End-to-end bot behavior | DM/group handling, media, allowlist, reply formatting, session creation, native commands |
| `extensions/telegram/src/bot.create-telegram-bot.test.ts` (134KB) | Bot creation/wiring | Account config, media groups, channel posts, command menus |
| `extensions/telegram/src/polling-session.test.ts` (76KB) | Long-polling lifecycle | Restart on error, stall detection, backoff timing, webhook clearing, graceful stop |
| `extensions/telegram/src/bot-message-dispatch.test.ts` (102KB) | Message dispatch | Session resolution, prompt building, reply delivery, draft streaming, error policies |
| `extensions/telegram/src/webhook.test.ts` (34KB) | Webhook HTTP server | Secret validation, rate limiting, concurrent requests, graceful drain |
| `extensions/telegram/src/send.test.ts` (99KB) | Outbound delivery | Chunking, markdown formatting, media sending, reply threading, error handling |

### Patterns Relevant to C Implementation

1. **Session-per-chat:** Each Telegram chat maps to one session file. Key format: `telegram:{dm|group}:{chatId}`. Simple key→file mapping.

2. **Serial execution per session:** Only one agent run per session at a time. Use a mutex per session.

3. **The core loop is simple:** Build system prompt → append user message → call LLM (streaming) → parse tool calls → execute tools → append results → loop until text-only → deliver reply.

4. **Transcript is append-only JSONL:** Messages appended to `.jsonl` file. On overflow, compaction replaces old messages.

5. **Telegram transport is just HTTP:** `getUpdates` long-polling with offset tracking, `sendMessage` for replies. No WebSocket needed.

6. **Streaming is optional:** CClaw can start non-streaming (wait for full response, send once).

7. **Error handling priorities:** (a) Rate limit retry with backoff, (b) context overflow detection, (c) abort signal propagation.

8. **Update offset persistence:** Store last `update_id + 1` in a file. Read on startup.

9. **Exponential backoff:** On transient errors: `delay = min(initial * factor^attempt * (1 + jitter*random), max_delay)`.

10. **Message chunking:** Telegram has 4096 char limit. Split long responses at paragraph/sentence boundaries.
