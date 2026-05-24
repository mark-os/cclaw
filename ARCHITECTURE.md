# CClaw — Architecture

## Overview

Minimal autonomous AI agent in C. Channels: Telegram, WhatsApp Business API, CLI, web dashboard. Runs on EC2 ARM64. SQLite for all persistence. MicroQuickJS for runtime tool creation. Sub-agents as separate processes sharing the same DB.

## Architecture

```
                    ┌─────────────────────────────────────┐
                    │           civetweb (HTTP)            │
                    │  WhatsApp webhooks · Dashboard · API │
                    └──────────────┬──────────────────────┘
                                   │ inbox_insert + signal
┌──────────┐   ┌──────────────┐   │   ┌──────────────┐
│ CLI mode │   │ Telegram poll│   │   │  Cron thread │
│ (in-proc │   │   (thread)   │   │   │  (thread)    │
│  no dmn) │   └──────┬───────┘   │   └──────┬───────┘
└──────────┘          │           │           │
                      ▼           ▼           ▼
               ┌─────────────────────────────────┐
               │         Signal Pipe             │
               │  (session_id wakeup channel)    │
               └──────────────┬──────────────────┘
                              │
               ┌──────────────▼──────────────────┐
               │           DAEMON                │
               │  epoll: signal_pipe + SIGCHLD   │
               │  fork agent │ reap child │      │
               │  deliver response to channel    │
               └──────┬────────────┬─────────────┘
                      │            │
            ┌─────────▼──┐  ┌─────▼──────────┐
            │ Agent Proc │  │ Agent Proc     │
            │ (forked)   │  │ (forked)       │
            │ landlock   │  │ landlock       │
            │ setrlimit  │  │ setrlimit      │
            │ drain inbox│  │ drain inbox    │
            │ LLM loop   │  │ LLM loop      │
            │ write resp │  │ write resp     │
            │ exit       │  │ exit           │
            └─────┬──────┘  └──────┬─────────┘
                  │                │
                  ▼                ▼
           ┌─────────────────────────────┐
           │          SQLite             │
           │  WAL mode · inbox · entries │
           │  sessions · FTS5 · cron     │
           └─────────────────────────────┘
```

### Daemon + Fork Model

- **Daemon process** (default when telegram_token or web port configured):
  - Main thread: epoll loop on signal_pipe fd + SIGCHLD self-pipe
  - On signal_pipe read: check if session idle → fork agent process
  - On SIGCHLD: `waitpid(WNOHANG)` → reap child → deliver response → check inbox → maybe re-fork
  - Telegram thread: long-polls getUpdates, does `inbox_insert` + writes session_id to signal pipe
  - Cron thread: fires scheduled tasks via `inbox_insert` + signal pipe
  - Civetweb: manages own thread pool for HTTP webhooks + dashboard (inserters signal pipe)
  - Daemon ⊥ executes LLM logic, tool dispatch, or agent loop code
- **Agent process** (forked by daemon per session turn):
  - Applies landlock (workspace write, system read-only)
  - Applies setrlimit (memory, CPU, file descriptors)
  - Acquires session lock (CAS safety net)
  - Drains inbox → appends as entries → updates `last_route`
  - Runs agent loop: LLM call → tool exec → repeat until `stop_reason == "stop"`
  - Writes final response to entries table
  - Releases lock, exits
- **CLI mode** (`cclaw --cli` or no channels configured):
  - Single process, no daemon, runs agent loop in-process on stdin/stdout
  - Does `inbox_insert` before lock acquire (message not lost on contention)
  - If lock fails: "queued — session is busy" (daemon-managed session)
- **Sub-agents**: agent calls `spawn_agent` → tool posts spawn request to dispatch queue → daemon forks sub-agent process → sub-agent writes result to parent inbox → daemon reaps → parent re-forks on next turn

### SQLite as Backbone

Everything lives in SQLite:
- Session tree (entries with parent_id, branching, leaf tracking)
- Message history (JSON payloads in entry rows)
- Full-text search (FTS5 over message content)
- Cron/scheduled tasks
- Sub-agent status and results
- Config overrides (per-session model, system prompt)

WAL mode + busy_timeout=5000ms allows multiple processes (daemon, CLI, sub-agents) to share one DB safely. Readers never block writers. Writers serialize briefly on commit.

### Memory Model

- **Session in memory**: only the active branch (loaded from SQLite on demand)
- **Per-turn arena**: 512KB scratch, created/destroyed each turn
- **Config**: 4KB arena, process lifetime, read-only after load

## Config

Config file (`config.json`) + env var overrides. Minimal defaults — just an API key gets you running.

```json
{
  "provider": "openrouter",
  "providers": {
    "openrouter": {
      "api_key": "$OPENROUTER_API_KEY",
      "base_url": "https://openrouter.ai/api/v1",
      "model": "deepseek/deepseek-v4-flash"
    }
  },
  "system_prompt": "You are CClaw, a minimal autonomous AI agent.",
  "max_history": 50,
  "max_tool_iterations": 10,
  "telegram_token": "",
  "whatsapp_verify_token": "",
  "whatsapp_access_token": "",
  "web_port": 8080,
  "db_path": "cclaw.db",
  "workspace": "./workspace"
}
```

Env vars: `OPENROUTER_API_KEY`, `CCLAW_PROVIDER`, `CCLAW_MODEL`, `CCLAW_TELEGRAM_TOKEN`, `CCLAW_DB_PATH`, `CCLAW_WEB_PORT`.

## Tools

### Built-in (C)

| Tool | Description |
|------|-------------|
| `shell_exec` | Run a shell command, return stdout/stderr |
| `file_read` | Read file contents (workspace-restricted) |
| `file_write` | Write/overwrite file (workspace-restricted) |
| `js_eval` | Execute JavaScript in sandboxed MicroQuickJS |
| `js_define_tool` | Define a new tool via JS function (persists for session) |
| `spawn_agent` | Fork a sub-agent process for a background task |
| `check_agent` | Check sub-agent status/result |

### Runtime-defined (JS via MicroQuickJS)

Agent can create new tools at runtime via `js_define_tool`. These are:
- Sandboxed (no filesystem, no network — use shell_exec/file_read for that)
- Session-persistent (replayed from history on session reload)
- Available to the agent on subsequent turns

MicroQuickJS constraints: 1MB heap cap, 10M instruction limit per eval, ES2020 subset (no async/await, no generators).

## Session Tree

Entries stored as a tree (id + parent_id). Current conversation = walk from leaf to root.

See [`notes/SCHEMA.md`](notes/SCHEMA.md) for full DB schema (current + planned).

Key tables: `sessions` (state machine, leaf tracking), `entries` (JSON payloads, tree structure, FTS5), `inbox` (durable message queue), `sub_agents`, `cron_jobs`, `js_tools`.

Operations:
- **append**: add entry, update leaf
- **get_branch**: walk leaf→root, reverse for chronological order
- **fork**: set leaf to an earlier entry, future appends branch off
- **navigate**: switch to a different branch
- **compact**: summarize old entries, replace with compaction entry

Only the active branch is loaded into memory. SQLite holds everything else.

## Channels

### Telegram
- Long-poll `getUpdates` in dedicated thread
- `sendMessage` + `sendChatAction("typing")` from worker threads
- Offset persisted to DB (survives restart)
- chat_id → session_id mapping

### WhatsApp Business API
- Webhook endpoint via civetweb: `POST /webhook/whatsapp`
- Verify endpoint: `GET /webhook/whatsapp?hub.verify_token=...`
- Send via Graph API (`POST https://graph.facebook.com/v21.0/{phone_id}/messages`)
- phone_number → session_id mapping

### CLI
- Separate process (`cclaw --cli`), stdin/stdout
- Creates/resumes a session in the shared SQLite DB
- No web server, no Telegram — just the agent loop
- Debug mode (`cclaw --cli --debug`) prints raw LLM requests/responses

### Web Dashboard
- Served by civetweb on configurable port
- Shows: active sessions, sub-agent status, recent messages, cron jobs
- Simple HTML — no JS framework, server-rendered or minimal vanilla JS
- Future: web-based chat interface

## Autonomy Features

### Heartbeats
- Timer thread wakes agent on configurable cadence (default: 30min)
- Injects system message: check tasks, report updates
- Agent can choose to act or skip

### Cron / Scheduled Tasks
- `cron_set(task, schedule, one_shot)` tool
- SQLite table tracks jobs + next_run_at
- Scheduler thread checks every 60s, injects task into appropriate session

### Sub-Agents
- Agent calls `spawn_agent` tool → tool writes spawn request to dispatch queue (SQLite table)
- Daemon picks up spawn request, forks sub-agent process, tracks parent↔child relationship
- Sub-agent: own session, landlock'd, runs until stop, writes result to parent inbox
- When sub-agent exits: daemon reaps, sees parent inbox has items, parent is idle → forks parent
- Blocking mode: parent process suspends (waits on eventfd); daemon writes to eventfd when sub-agent completes
- Background mode: parent continues; sub-agent posts result to parent inbox on completion
- Limits: max 3 concurrent per parent, max 10 system-wide, max depth 2

## Implementation Notes (from Pi/OpenClaw reference)

### Agent Loop Pattern (Pi)

Two nested loops:
- **Outer loop**: handles follow-up messages (agent finished but new input arrived)
- **Inner loop**: call LLM → if tool_calls, execute them, loop back. Exit when response has no tool_calls.

```
while (follow_up || steering_message):
    while (has_tool_calls || pending_messages):
        inject pending messages into context
        call LLM → get response
        if error/abort: return
        extract tool_calls from response
        if tool_calls: execute all → append results → continue
        else: break
    check for follow-up messages → if any, continue outer
    else: done
```

### Tool Execution Pipeline

1. Find tool by name (registry lookup) → not found = error result
2. Validate arguments against schema (optional in Phase 1)
3. Execute tool function → capture output
4. On exception/timeout → error result (never crash the loop)
5. Append tool result message to context

Batch: if LLM returns multiple tool_calls, execute all, append all results, then call LLM again. Terminate only when ALL results say terminate.

### Telegram Patterns (OpenClaw)

- **Offset persistence**: store `last_update_id + 1` in DB. Read on startup.
- **Message chunking**: 4096 char limit. Split at paragraph boundaries, fall back to sentence boundaries.
- **Typing indicator**: send `sendChatAction("typing")` every 4 seconds while agent is working.
- **Exponential backoff**: on transient errors: `delay = min(2s * 1.8^attempt * (1 + 0.25*random), 30s)`
- **Serial per session**: mutex per chat_id. Queue messages if agent is mid-turn.

### Error Handling

- **Rate limit (429)**: respect `Retry-After` header, retry with backoff
- **Context overflow**: detect via error message pattern matching, trigger compaction, retry
- **Abort**: set flag, close curl handle, set stop_reason = "aborted"
- **JSON parse failure from LLM**: inject error as tool result, let LLM self-correct
- **Tool crash**: catch (in C: check return code), produce error result, continue loop
- **Missing finish_reason**: treat as error

### StopReason Normalization (Pi model)

Every LLM response is normalized to a `StopReason` enum before storage:

```
stop      — normal completion (provider: "stop", "end", "end_turn", null)
length    — hit max tokens (provider: "length", "max_tokens")
tool_use  — wants to call tools (provider: "tool_calls", "function_call", "tool_use")
error     — provider error, content_filter, parse failure, missing finish_reason
aborted   — client-side abort (SIGTERM, user cancel, RLIMIT_CPU)
```

`map_stop_reason()` in `llm.c` is the sole normalization point. Stored in entry `data` JSON as `"stop_reason"` field.

**Context filtering (V36)**: `context_build` skips assistant entries with `stop_reason ∈ {error, aborted}`. Their orphaned tool_calls get synthetic error results. This prevents replaying broken turns to the LLM (which can cause API errors with some providers).

### Key C Patterns

- Content is always an array of tagged-union blocks (text, tool_call, thinking)
- Strip assistant messages with `stop_reason ∈ {error, aborted}` when building LLM request (V36)
- Never send `"tools": []` — omit the field entirely if no tools
- Tool call arguments arrive as a JSON string — parse with cJSON on use
- Usage: `input_tokens = prompt_tokens - cached_tokens`
