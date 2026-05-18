# Phase 2 — Telegram + Persistence + Sub-Agent Spawn

## Goal

Bot runs as a daemon, responds on Telegram, persists conversation history to SQLite (tree-ready schema), and can spawn minimal background sub-agents.

## Prerequisites

Phase 1 complete: working CLI agent with shell_exec tool, in-memory message history.

## Components

### 1. SQLite Persistence

Schema from SESSION_TREE.md. Operations:

- `db_init(path)` — open DB, create tables, WAL mode, busy_timeout=5000
- `db_create_session(name)` → session_id
- `db_append_entry(session_id, parent_id, type, payload_json)` → entry_id
- `db_get_branch(session_id)` → Entry[] (walk from leaf to root, reverse)
- `db_set_leaf(session_id, entry_id)` — update session_state
- `db_delete_from_end(session_id, n)` — remove last n entries, update leaf
- `db_list_sessions()` → SessionMetadata[]

Flush strategy: append to SQLite after each assistant turn completes (not mid-stream). On startup, load the full branch into memory.

### 2. Config File

Parse `config.json` with cJSON. Env vars override file values.

```json
{
  "api_key": "...",
  "base_url": "https://integrate.api.nvidia.com/v1",
  "model": "deepseek-ai/deepseek-v4-flash",
  "system_prompt": "You are CClaw, a minimal AI agent...",
  "max_history": 50,
  "max_tool_iterations": 10,
  "telegram_token": "...",
  "db_path": "cclaw.db"
}
```

Env var mapping: `CCLAW_API_KEY`, `CCLAW_MODEL`, `CCLAW_BASE_URL`, `CCLAW_TELEGRAM_TOKEN`, `CCLAW_DB_PATH`.

### 3. Telegram Channel

Minimal Telegram bot using raw HTTP (libcurl). No bot framework.

**Polling loop** (own thread):
1. `getUpdates(offset, timeout=30)` — long-poll
2. For each update: extract chat_id, text, update_id
3. Push `{chat_id, text}` to dispatch queue
4. Update offset = max(update_id) + 1
5. Persist offset to file (survives restart)

**Send** (from worker threads):
- `sendMessage(chat_id, text)` — split at 4096 chars if needed
- `sendChatAction(chat_id, "typing")` — while agent is thinking

**Error handling:**
- Network error → retry with exponential backoff (2s initial, 30s max)
- 429 → respect Retry-After header
- Persist offset so no messages are reprocessed on restart

### 4. Multi-Channel Dispatcher

```
main thread
  ├── CLI mode (if no telegram_token): direct agent loop on stdin/stdout
  └── Daemon mode (if telegram_token set):
       ├── Telegram poll thread → pushes to queue
       └── Worker thread pool (N=2 initially)
            └── pop from queue → resolve session → agent_turn → send reply
```

Thread-safe dispatch queue: mutex + condvar, bounded ring buffer.

Session isolation: each chat_id maps to one session. Mutex per session ensures only one turn runs at a time. If a message arrives while a turn is running, it queues.

Graceful shutdown: SIGINT/SIGTERM → set `volatile int shutdown = 1` → poll thread exits → workers drain queue → close DB.

### 5. File Tools

Two new tools registered alongside shell_exec:

**file_read(path, max_lines?)** — read file contents, cap at 64KB output
**file_write(path, content)** — write/overwrite file

Both restricted to a workspace directory (configurable, default: `./workspace/`). Reject paths outside workspace (no `../` traversal).

### 6. Minimal Sub-Agent Spawn

A `spawn_agent` tool available to the main agent:

```json
{
  "name": "spawn_agent",
  "description": "Spawn a background agent to work on a task independently",
  "parameters": {
    "task": "string — what the sub-agent should do",
    "system_prompt": "string — optional override system prompt"
  }
}
```

Implementation:
1. Create a new session in SQLite (`parent_session_id` = current session)
2. Spawn a new thread running the agent loop with:
   - Its own session (separate entry tree)
   - The task as the initial user message
   - Same tools as parent (shell_exec, file_read, file_write)
   - Max iterations limit (e.g., 20 turns)
3. Return immediately with `{"agent_id": "...", "status": "running"}`
4. Sub-agent runs to completion or hits iteration limit
5. Parent can check status via `check_agent(agent_id)` tool

Limits:
- Max 3 concurrent sub-agents per parent session
- Max spawn depth of 1 (sub-agents cannot spawn sub-agents in Phase 2)
- Sub-agent inherits workspace directory

### Beads Breakdown

| Bead | Description | Depends on |
|------|-------------|-----------|
| P2-config | Config file parsing (cJSON) + env var override | P1 core types |
| P2-sqlite | SQLite persistence with tree-ready schema | P1 core types |
| P2-telegram-poll | Telegram getUpdates polling + offset persistence | P2-config |
| P2-telegram-send | sendMessage + sendChatAction + chunking | P2-config |
| P2-dispatcher | Thread pool + dispatch queue + session routing | P2-sqlite, P2-telegram-poll |
| P2-file-tools | file_read + file_write with workspace restriction | P1 tool framework |
| P2-spawn | spawn_agent + check_agent tools | P2-sqlite, P2-dispatcher |

## Testing Strategy

- SQLite: unit tests with temp DB files
- Telegram: mock HTTP responses (inject a fake curl handler or use a local HTTP server)
- Dispatcher: unit test the queue (multi-threaded push/pop)
- File tools: unit test with temp directories
- Spawn: integration test — spawn agent with mock LLM, verify session created and result stored
- End-to-end: pipe messages through the system with a mock Telegram server

## Open Questions for Implementation

1. Should sub-agent results be injected back into the parent session automatically, or only on explicit `check_agent` call?
2. Typing indicator cadence — send every 4s while agent is working? (OpenClaw does this)
3. Should the workspace directory be per-session or global?
