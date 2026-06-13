# Architecture

## Process Model

CClaw has two execution modes sharing the same binary:

**CLI mode** (default): single process with a worker thread pool for LLM calls. Poll loop handles stdin, worker completions, and tool child processes. Opens `~/.cclaw/cclaw.db` directly.

**Daemon mode** (`--daemon`): same process model plus channel management (Telegram, webhooks), cron scheduling, web dashboard, and multi-agent coordination. Worker threads handle concurrent sessions.

```
Main process
├─ main thread: poll loop (stdin/wake/fifo, signals, worker notifications)
├─ worker threads (1–N elastic, each owns sqlite3* + CURL*)
│    LLM HTTP calls, connection reuse, 30s idle timeout
│
├─ fork+exec → shell children (sandboxed: namespaces, no direct network)
├─ fork+exec → channel runners (long-lived, per messaging platform)
├─ civetweb thread (webhooks, dashboard)
├─ heartbeat thread
└─ cron scheduler thread
```

## Single DB

All state lives in `cclaw.db` (WAL mode). Sessions, entries, config, memory, secrets, job queue — one file, one source of truth. WAL allows concurrent reads/writes across threads without blocking.

See [specs/schema.md](../specs/schema.md) for full DDL.

## Turn Lifecycle

```
1. Message arrives (Telegram, cron, webhook, sub-agent completion, CLI stdin)
2. inbox_insert(session_id, source, payload)
3. Wake notification → poll loop wakes
4. advance_session(session_id) — state machine decides next action:
   - DISPATCH_LLM → submit to worker thread pool
   - DISPATCH_TOOLS → fork tool children or run inline
   - DONE → deliver response
5. Worker thread: llm_req() → HTTP call → write entry to DB → notify main
6. Main thread: run_advance() → check for tool calls → dispatch or deliver
7. Tool children: fork, execute, write result via pipe → reap → advance again
8. Repeat until stop_reason == "stop" with no pending tool calls
```

## LLM Request Pipeline

### Two-Pass Context Building

1. **Plan pass** (index-only): Recursive CTE walks `parent_id` from `leaf_id` to root, collecting entry metadata. Covered entirely by index — no content loaded.

2. **Cut**: Walk from leaf toward root, accumulating token estimates until budget exhausted. Tool-call groups kept/dropped as a unit.

3. **Payload pass**: Surviving entry IDs joined against `entries` to produce JSON payload via SQLite `json_object()`/`json_group_array()`. Zero-copy: payload body points into SQLite statement buffer until `llm_payload_release()`.

### Prompt Caching

Providers offer prefix-based caching automatically. CClaw benefits because:
- Payload is deterministic (same entries → same JSON, byte-for-byte)
- Conversations grow by appending — prefix is stable
- Recall text appended after messages, doesn't break cache

## State Machine

Session state drives the event loop via `advance_session()`:

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

Special states: `waiting` (blocked on sub-agent), `rate_limited`.

## Security Layers

| Layer | Protects | Enforced by |
|-------|----------|-------------|
| setrlimit | Resource exhaustion | Kernel (main process) |
| http_check_policy | Network exfiltration | Application (LLM calls) |
| Namespace sandbox | Filesystem + network | Kernel (shell children) |
| UDS proxy | Host allowlist for shell | Application (proxy thread) |
| Env stripping | Secret leakage to shell | Application (fork) |
| Encrypted secrets | Disk theft | ChaCha20-Poly1305 (cclaw.db) |

See [specs/security.md](../specs/security.md) for full details.

## First-Run Flow

1. User runs `cclaw` — no `~/.cclaw/` exists
2. Create `~/.cclaw/`, generate `.cclaw_key` (32 random bytes, mode 0600)
3. Create `cclaw.db` with schema, seed default config
4. If `OPENROUTER_API_KEY` in env → encrypt and store in cclaw.db kv
5. Create default agent + `agents/default/workspace/`
6. Enter agent loop — user is chatting immediately
