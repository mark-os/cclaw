# Architecture

## Process Model

CClaw has two execution modes sharing the same binary:

**CLI mode** (default): one process with a worker thread pool for LLM calls. A poll loop handles stdin, worker completions, and tool child processes. Opens `~/.cclaw/cclaw.db` directly.

**Daemon mode** (`--daemon`): the same process model plus channels (Telegram, Discord, WhatsApp, webhooks), cron, the web dashboard, and many agents working at once. Worker threads handle concurrent sessions.

```
Main process
├─ main thread: poll loop (stdin, wake fd, signals, worker notifications,
│                periodic DB tick: cron, recovery, approvals)
├─ LLM worker threads (1–N elastic, each owns sqlite3* + CURL*)
│    blocking HTTP calls, connection reuse, 30s idle timeout
├─ tool threads (DB-only tools: one short-lived thread per call)
│
├─ fork+exec → cclaw --run-tool   (one per sandboxed tool call: file, web, JS, shell)
├─ fork+exec → cclaw --channel    (daemon: one long-lived runner per channel)
└─ civetweb threads               (daemon: webhooks, dashboard)
```

There is no separate heartbeat or cron thread: the agent's heartbeat is an ordinary cron job, and due cron jobs fire from the main loop's periodic DB tick.

Every sandboxed tool call — file tools, `web_fetch`, JavaScript and the shell — runs in a `cclaw --run-tool` child, re-executed from the same binary so it starts with a clean address space. See [specs/tool-execution-model.md](../specs/tool-execution-model.md).

## Single DB

All state lives in `cclaw.db` (WAL mode). Sessions, entries, config, memory, secrets, job queues — one file, one source of truth. WAL lets threads and processes read and write concurrently without blocking each other.

See [specs/schema.md](../specs/schema.md) for full DDL.

## Turn Lifecycle

A **turn** runs from the moment queued input is consumed to the final assistant message being delivered. Within a turn, each LLM request and the tool calls it asks for make one **iteration**.

```
1. Message arrives (channel, cron, webhook, sub-agent result, CLI stdin)
2. inbox_insert(session_id, source, payload)
3. Wake notification → poll loop wakes
4. advance_session(session_id) reads session state and decides the next action:
   - DISPATCH_LLM   → submit to the worker thread pool
   - DISPATCH_TOOLS → run tools inline, on a tool thread, or in a --run-tool child
   - DONE           → deliver the response
5. Worker thread: llm_req() → HTTP call → write entry to DB → notify main
6. Main thread: run_advance() → check for tool calls → dispatch or deliver
7. Tool results are written to the DB → the loop is notified → advance again
8. Repeat until the model stops with no pending tool calls
```

Mid-turn, only LLM output and the results of this turn's tool calls are written to the conversation. Everything else (channel messages, cron fires, sub-agent results) waits in the inbox and enters at the next turn boundary.

## LLM Request Pipeline

### Two-Pass Context Building

1. **Plan pass** (index-only): a recursive CTE walks `parent_id` from `leaf_id` to the root, collecting entry metadata. The index covers it entirely — no content is loaded.

2. **Cut**: walk from the leaf toward the root, adding up token estimates until the budget runs out. The cut snaps back to the start of a turn, so a turn is never split.

3. **Payload pass**: the surviving entry IDs are joined against `entries`, and SQLite's `json_object()`/`json_group_array()` produce the request body. Zero-copy: the body points into the SQLite statement buffer until `llm_payload_release()`.

### Prompt Caching

Providers cache on a stable request prefix. CClaw keeps the prefix stable because:
- The payload is deterministic (same entries → same JSON, byte for byte)
- Conversations grow by appending
- The session context block (recall and live state) is computed once per turn and placed just before the newest user message, so it never moves mid-turn

## State Machine

Session state drives the loop via `advance_session()`:

```
idle ──[inbox has work]──→ llm_running ──[tool_calls]──→ tool_running
  ↑                            │                              │
  │                            └──[stop, no tools]────────────┘
  │                                                           │
  └───────────────────────[all tools done]────────────────────┘
                               ↓
                          compacting (optional)
                               ↓
                             idle
```

Other states: `awaiting_approval` (a tool call is parked for a human decision) and `rate_limited`. A parent that launched a blocking sub-agent stays parked until the child's result arrives.

## Security Layers

| Layer | Protects | Enforced by |
|-------|----------|-------------|
| setrlimit | Resource exhaustion | Kernel (tool children) |
| Namespace sandbox | Filesystem + network | Kernel (tool children) |
| Egress proxy | Host allowlist for tool traffic | Application (`host_decide()` in the per-call proxy) |
| Env stripping | Secret leakage to tool children | Application (fork) |
| Secret scanner | Credentials entering the context | Application (tool results, user messages) |
| Encrypted secrets | Disk theft | ChaCha20-Poly1305 (cclaw.db) |

Under the `host` sandbox profile the namespace sandbox and the egress proxy are both skipped. See [specs/security.md](../specs/security.md) and [specs/sandbox-profiles.md](../specs/sandbox-profiles.md).

## First-Run Flow

1. User runs `cclaw` — no `~/.cclaw/` exists
2. Create `~/.cclaw/`, generate `.cclaw_key` (mode 0600)
3. Create `cclaw.db` with the schema; seed providers, models and the config registry
4. The API key is read from the environment (`OPENROUTER_API_KEY` by default) or the encrypted `secrets` table — it is not copied into the DB
5. Create the default agent `Assistant` and `agents/Assistant/workspace/`
6. Enter the agent loop — the user is chatting immediately
