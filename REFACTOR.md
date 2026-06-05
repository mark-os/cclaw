# REFACTOR.md — Architecture Redesign

## Overview

CClaw becomes a single parent process (CLI or daemon) that manages all agents
via fork+exec. The agent loop moves from in-process to a DB-mediated
parent-orchestrated cycle. All state lives in a single SQLite database.

**Reference docs:**
- [specs/provider-schemas.md](specs/provider-schemas.md) — per-provider wire
  formats (request + response, streaming + non-streaming) that define the
  target output of SQL request builders and input format for jsmn parsers
- [reference/pi-ai.md](reference/pi-ai.md) — Pi agent LLM layer reference

## Core Decisions

### 1. Single Parent Process

The parent (cclaw binary) is the only long-lived process. It:
- Manages all agents, sessions, and config
- Owns the single SQLite database (single writer)
- Orchestrates the agent turn loop by forking children
- Reads child results from the DB after each fork+exec completes

No in-process agent loop. No threads. Just fork, wait, read DB, decide, repeat.

### 2. Fork+Exec Everything

Each unit of work is a short-lived child process:

| Child | Job | Sandbox |
|-------|-----|---------|
| `cclaw-llm` | Read session from DB → build request → stream response → write to DB | No (network-only, single purpose) |
| `cclaw-tool-*` | Receive args via argv/stdin → execute → write result to stdout | Yes (seccomp/landlock) |

The parent's turn loop:
```
while turn not complete:
    fork+exec cclaw-llm (reads DB, calls LLM, writes response + tool_calls to DB)
    wait for exit
    if non-zero exit: write error entry, let LLM recover next turn
    read pending tool_calls from DB
    if no tool_calls: break (deliver response)
    for each tool_call:
        if name ∈ {spawn_agent, approval_request, configure_*}:
            handle in parent (write result to DB)
        else:
            fork+exec cclaw-tool (receives args via argv/stdin, executes, writes result to stdout)
            parent reads stdout pipe, writes result to DB
            wait for exit (or timeout)
    loop (next LLM call with tool results)
```

### 3. DB as the Only IPC

Children communicate with the parent via DB or pipe:
- LLM child reads entries from DB, writes new entry + tool_calls rows to DB
- Tool child receives args on stdin/argv, writes result to stdout (no DB access)
- Parent captures tool stdout via pipe, writes result to DB
- Parent reads the DB after each child exits to decide next step
- No exit code signaling — just 0 (success) or non-zero (crash)

LLM child has DB access (unsandboxed). Tool children are sandboxed — no
filesystem beyond workspace, no network, no DB. Stdout pipe is the only
output channel.

Dispatch is data-driven: parent reads `tool_calls` table, decides per-name
whether to handle in-process (special tools) or fork a child (regular tools).

### 4. JSON Strategy

| Operation | Method | Rationale |
|-----------|--------|-----------|
| Request building (egress) | SQLite `json_object()` / `json_group_array()` | Data already in DB; eliminates C emitter code |
| Streaming SSE parsing | jsmn (vendored) | Zero-alloc, per-chunk speed |
| Non-streaming response parsing | SQLite `json_extract()` | Parse + store in one INSERT |
| Tool call arg validation | SQLite `json_valid()` / `json()` | Validates at write time |
| Tool call arg reading | SQLite `json_extract()` | Parent extracts fields for dispatch |
| JSON construction (Telegram, web) | cJSON (remains) | Builder API for outbound JSON |

### 5. Request Building via SQL

One SQL query per provider format produces the complete request body:

```sql
-- OpenAI format
SELECT json_object(
  'model', ?2,
  'messages', json_group_array(json(msg)),
  'stream', json('true'),
  'stream_options', json_object('include_usage', json('true'))
) FROM (
  SELECT json_object('role', ..., 'content', ...) AS msg
  FROM entries WHERE session_id = ?1 AND ...
  ORDER BY id
);
```

Provider-specific queries for: OpenAI, Gemini, Anthropic.
The C code just binds params and passes the result string to curl.

### 6. Cache Break Markers

Column on sessions table: `cache_break_after INTEGER` (entry ID).
Updated after each complete turn. Request builder SQL conditionally emits
`cache_control` on the entry matching that ID.

No marker rows. No fragmentation. One UPDATE per turn.

### 7. Response Ingress

**Streaming path (primary):**
- `cclaw-llm` child opens SSE connection
- Parses chunks with jsmn into `SseChunk` struct
- Streams tokens to stdout (parent can display or discard)
- Accumulates content, tool_calls, usage
- On stream end: writes single INSERT to DB

**Non-streaming path (fallback):**
- `cclaw-llm` child receives full response body
- Single SQL statement extracts fields and inserts:

```sql
INSERT INTO entries(session_id, role, content, tool_calls, finish_reason, usage_json)
VALUES(?1, 2,
  json_extract(?2, '$.choices[0].message.content'),
  json_extract(?2, '$.choices[0].message.tool_calls'),
  json_extract(?2, '$.choices[0].finish_reason'),
  json_extract(?2, '$.usage')
);
```

### 8. Tool Call Storage

Tool calls written to DB as validated JSON:

```sql
-- LLM child writes (after response parsing):
INSERT INTO tool_calls(session_id, entry_id, call_id, name, arguments)
VALUES(?1, ?2, ?3, ?4, json(?5));
```

`json(?5)` validates + normalizes. If LLM emits malformed args, `json()`
returns NULL → parent detects and sends error result back to LLM.

Parent reads for dispatch and passes to child:
```sql
SELECT call_id, name, arguments
FROM tool_calls WHERE entry_id = ? AND session_id = ?;
```

Parent passes `name` and `arguments` to the tool child via argv/stdin.
Child writes result to stdout. Parent captures and inserts:
```sql
INSERT INTO entries(session_id, role, content, tool_call_id)
VALUES(?1, 3, ?2, ?3);
```

### 9. Per-Provider SSE Parsers (jsmn)

Replace the monolithic `sse_process_line()` with per-provider functions:

```c
typedef struct {
    const char *text;       size_t text_len;
    const char *reasoning;  size_t reasoning_len;
    int is_thinking;
    const char *tc_name;    size_t tc_name_len;
    const char *tc_id;      size_t tc_id_len;
    const char *tc_args;    size_t tc_args_len;
    int tc_args_complete;   /* 1=Gemini (full obj), 0=OpenAI/Anthropic (fragment) */
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

Stack-allocated token buffer (256 tokens max). Zero heap allocation per chunk.

### 10. What Gets Removed

- `request_stream.c` — entire streaming request builder (replaced by SQL queries)
- `emit_entry_gemini`, `emit_entry_openai`, `emit_tool_calls_*` functions
- `json_escape_into` for request building (SQLite handles escaping)
- In-process agent loop in `agent.c` (replaced by parent fork+wait loop)
- cJSON usage in `llm.c` response parsing (replaced by jsmn/SQLite)
- cJSON usage in `tool_*.c` argument parsing (replaced by SQLite json_extract)

### 11. What Remains

- cJSON: Telegram bot JSON construction, webhook payloads, config file parsing
- SQLite: single DB, all state, JSON operations for request/response
- jsmn: streaming SSE chunk parsing only
- curl: HTTP client in `cclaw-llm` child

### 12. Binary Structure

```
cclaw              — parent process (orchestrator, DB owner, CLI/daemon)
cclaw-llm          — child: LLM request/response (unsandboxed, network access)
cclaw-tool         — child: tool execution (sandboxed, restricted)
```

Could be a single binary with subcommands (`cclaw llm`, `cclaw tool`) or
separate binaries. Single binary with argv[0] dispatch is simpler to deploy.

### 13. DB Schema Changes

```sql
-- Add to sessions:
ALTER TABLE sessions ADD COLUMN cache_break_after INTEGER;

-- New table for explicit tool call tracking:
CREATE TABLE IF NOT EXISTS tool_calls (
    id INTEGER PRIMARY KEY,
    session_id INTEGER NOT NULL,
    entry_id INTEGER NOT NULL,      -- which assistant entry produced this
    call_id TEXT NOT NULL,           -- LLM-assigned ID
    name TEXT NOT NULL,
    arguments TEXT,                  -- validated JSON (via json())
    result_entry_id INTEGER,        -- entry with tool result (NULL until executed)
    status INTEGER DEFAULT 0        -- 0=pending, 1=running, 2=done, 3=error
);
```

### 14. Tasks

Corresponds to SPEC.md §T T294–T301. Each phase produces a working system.

#### T294: Vendor jsmn + per-provider SSE parsers

- Vendor `jsmn.h` (single header, public domain, ~400 LOC) into `vendor/jsmn/`
- Implement `sse_parse_openai()` — extract `choices[0].delta.content`,
  `delta.reasoning_content`, `delta.tool_calls[]`, `finish_reason`, `usage`
  via jsmn token traversal
- Implement `sse_parse_gemini()` — extract `candidates[0].content.parts[]`,
  detect `thought:true`, `functionCall`, `finishReason`, `usageMetadata`
- Implement `sse_parse_anthropic()` — handle typed events (`content_block_start`,
  `content_block_delta`, `content_block_stop`, `message_delta`); track blocks
  by index; accumulate `text_delta`, `thinking_delta`, `input_json_delta`
- Shared output struct `SseChunk` (pointers into source JSON, zero-copy)
- Stack-allocated jsmn token buffer (256 tokens max)
- Unit tests with captured real SSE chunks from each provider
- Drop-in replace current `sse_process_line()` in `http.c`
- Verify: existing streaming tests pass, live e2e with Gemini + OpenRouter

#### T295: SQL request builder queries

- Write `sql/request_openai.sql` — produces complete OpenAI chat completions
  JSON from entries table (`json_object`, `json_group_array`)
- Write `sql/request_gemini.sql` — produces native Gemini `contents[]` format
  with `systemInstruction`, `tools`, `generationConfig`
- Write `sql/request_anthropic.sql` — produces Anthropic messages format with
  content blocks, thinking replay, tool_use history
- Add `cache_break_after INTEGER` column to sessions table
- All queries conditionally emit `cache_control` on the entry matching
  `cache_break_after` (CASE expression, no null keys)
- Implement `db_build_request(db, session_id, endpoint_type, model, ...)` in C
  — binds params, executes query, returns result string
- Handle tool_calls in history: OpenAI stringified args, Gemini functionCall
  parts, Anthropic tool_use blocks
- Test: live API calls with SQL-built body succeed (Gemini flash-lite, OpenRouter)
- Remove `request_stream.c` dependency (can coexist initially behind flag)

#### T296: SQL response ingress + tool_calls table

- Add `tool_calls` table to schema:
  ```sql
  CREATE TABLE tool_calls (
      id INTEGER PRIMARY KEY,
      session_id INTEGER NOT NULL,
      entry_id INTEGER NOT NULL,
      call_id TEXT NOT NULL,
      name TEXT NOT NULL,
      arguments TEXT,          -- validated via json()
      result_entry_id INTEGER,
      status INTEGER DEFAULT 0 -- 0=pending, 1=running, 2=done, 3=error
  );
  ```
- Non-streaming response ingress: single INSERT using `json_extract()`:
  ```sql
  INSERT INTO entries(session_id, role, content, tool_calls, stop_reason, usage_json)
  VALUES(?1, 2,
    json_extract(?2, '$.choices[0].message.content'),
    json_extract(?2, '$.choices[0].message.tool_calls'),
    json_extract(?2, '$.choices[0].finish_reason'),
    json_extract(?2, '$.usage'));
  ```
- Tool call extraction: after entry INSERT, parse tool_calls JSON and INSERT
  into tool_calls table with `json()` validation on arguments
- Streaming ingress: jsmn accumulator builds content/tool_calls/usage, then
  single INSERT (same as non-streaming, just different source)
- Tool arg reading by parent: `SELECT call_id, name, arguments FROM tool_calls
  WHERE entry_id = ? AND status = 0`
- Test: round-trip — LLM response stored, tool_calls extracted, parent reads

#### T297: Merge 3 DBs into single cclaw.db

- Unified schema: sessions, entries, tool_calls, kv, memory_blocks, js_tools,
  inbox, agents, agent_config, providers, channels, channel_events,
  channel_outbox, channel_state, log — all in one file
- Add `agent_name TEXT` to sessions, entries, kv, memory_blocks (scope rows to
  agent within shared DB)
- Remove `db_open_agent()`, `db_open_journal()` — single `db_open()` call
- Parent opens DB once at startup, children inherit path via env
  (`CCLAW_DB_PATH`)
- `cclaw-llm` opens DB in WAL mode (read entries, write response)
- Journal/log table replaces journal.db (parent writes from stderr pipe)
- Remove `.cclaw/agents/<name>/agent.db` file structure
- Test: CLI and daemon both work with single DB file

#### T298: Extract `cclaw-llm` child

- New subcommand: `cclaw llm --session=N --db=PATH`
- Child responsibilities:
  - Open DB (WAL mode)
  - Context planning (token budget, cut point, entry selection)
  - Build request body via SQL query (T295)
  - Execute curl POST (streaming or non-streaming)
  - Parse response (jsmn for streaming, json_extract for non-streaming)
  - Write assistant entry + tool_calls rows to DB
  - Stream tokens to stdout (inherited from parent)
  - Exit 0 on success, 1 on error
- Parent responsibilities:
  - Fork + set up stderr pipe
  - `waitpid` (child inherits stdout directly)
  - Read stderr pipe → journal table
  - After exit: read DB for new entry (finish_reason, tool_calls)
- Config passed via env vars (same `CCLAW_*` pattern as today)
- Test: `cclaw llm --session=N` works standalone, tokens stream, DB updated

#### T299: Parent turn loop

- Orchestrator logic in parent (CLI and daemon share same code):
  ```
  loop:
      fork cclaw-llm (session_id)
      wait, journal stderr
      if non-zero exit: write error entry, break or retry
      read pending tool_calls from DB
      if none: break (deliver response)
      for each tool_call (serial):
          if name ∈ {spawn_agent, approval_request, configure_*}:
              handle in parent (write result to DB directly)
          else:
              fork cclaw-tool (name, arguments via stdin)
              capture stdout pipe → result
              wait
              INSERT tool result entry into DB
          UPDATE tool_calls SET status=2, result_entry_id=?
      loop
  ```
- Tool child interface: receives JSON arguments on stdin, writes result to
  stdout, exits 0/1
- Parent passes tool name + arguments, captures stdout as result string
- Special tools (spawn, approval, config) handled in-process by parent —
  it's the sole entity with authority for those mutations
- Error handling: tool timeout → kill child, write error result
- Exit codes: 0 = success, non-zero = crash (no custom codes)
- CLI mode: tokens stream directly (cclaw-llm inherits stdout)
- Daemon mode: tokens go to /dev/null or buffer (deliver final response via channel)
- Test: multi-turn with tool calls works end-to-end

#### T300: Sandbox with unshare

- Remove ALL landlock code (`landlock_*` calls, `landlock.h`)
- Remove ALL seccomp code (`seccomp_*`, BPF filters)
- Tool child sandbox via `unshare(2)`:
  - `CLONE_NEWNS` — mount namespace, bind-mount workspace rw + system dirs ro
  - `CLONE_NEWPID` — PID namespace (child is PID 1)
  - `CLONE_NEWNET` — network namespace (no network by default)
- Network access: parent sets up proxy, passes `http_proxy` env to child;
  child network goes through proxy which enforces `allowed_hosts`
- Per-agent config: `sandbox = "sandbox" | "none"`
- Default: `sandbox` for daemon agents, `none` for CLI
- Graceful fallback: if `unshare()` fails (unprivileged, kernel config) → run
  unsandboxed, log warning
- Test: tool child can only access workspace, network goes through proxy

#### T301: Remove old code

- Delete `src/request_stream.c` (replaced by SQL queries)
- Delete in-process agent loop in `agent.c` (`agent_run`, `agent_turn_run`)
- Delete old tool dispatch (in-process `dispatch_tool` → replaced by fork)
- Delete cJSON usage in `llm.c` response parsing
- Delete cJSON usage in `tool_*.c` argument parsing (parent extracts via SQL)
- Delete multi-DB open logic (`db_open_agent`, `db_open_journal`, 3-DB init)
- Delete all landlock/seccomp code
- Delete `libcclaw_net.so` preload library (replaced by unshare+proxy)
- Update Makefile: remove deleted source files, add jsmn vendor
- Verify: clean build, `make test` passes, `make test-integration` passes,
  `make test-e2e` passes, binary size reduced

### 15. Sandbox Configuration

Sandbox is per-agent, configurable. Uses `unshare(2)` for isolation (no
landlock, no seccomp):

| Level | Behavior |
|-------|----------|
| `none` | Tool child runs as user with full permissions. No namespace isolation. |
| `sandbox` | `unshare(CLONE_NEWNS, CLONE_NEWPID, CLONE_NEWNET)` — mount namespace (workspace bind mount), PID namespace, network namespace. |

Network access for sandboxed tools goes through the existing proxy model —
parent runs an HTTP proxy, tool child gets `http_proxy` env var pointing to
it. Parent enforces allowed hosts.

Default: `sandbox` for daemon-managed agents, `none` for CLI.

Stdout pipe is the uniform result interface regardless of sandbox level.

Config in agent definition:
```sql
-- Per-agent setting
sandbox = "sandbox" | "none"
```

What gets removed: all landlock code, all seccomp filters.

### 16. Streaming Token Display

`cclaw-llm` inherits the parent's stdout directly. Tokens stream to the
terminal in real-time without parent involvement. Parent captures stderr
via pipe for journal logging (same pattern as today's `cli_fork_turn`).

`cclaw-tool` stdout is captured by the parent via pipe (it's the tool result).
Tool stderr can be merged or captured separately for logging.

### 17. Context Planning

`cclaw-llm` is responsible for context planning. It reads the session from
the DB, computes the token budget / context window, decides which entries
to include (cut point), and builds the request body — all internally.

Parent does not compute or pass entry IDs. It just forks `cclaw-llm` with
the session ID and provider config.

### 18. Tool Parallelism

Serial for now. One tool call at a time, in order. The parent forks, waits,
writes result, then moves to the next tool call. Parallel execution is a
future optimization (fork all, wait all, write all).


### 19. Next Actions (spawn/approval/config)

Exit code signaling (0/2/3/4) is abandoned. The parent's dispatch is purely
data-driven after `cclaw-llm` exits:

```
after cclaw-llm exits 0:
    read pending tool_calls from DB
    for each tool_call:
        if name ∈ {spawn_agent, approval_request, configure_provider,
                    configure_channel, create_agent}:
            handle in parent (sole authority for these mutations)
        else:
            fork tool child (stdin=args, capture stdout=result)
```

`cclaw-llm` writes ALL tool calls to the `tool_calls` table before exiting —
including the "special" ones. It doesn't need to know which are special. The
parent reads the table and decides.

Special tool handling by parent:
- `spawn_agent` → fork another `cclaw-llm` for child session
- `approval_request` → notify admin, pause session until resolution
- `configure_*` / `create_agent` → validate + write config to DB

Exit codes reduced to:
- 0 = success (response + tool_calls written to DB)
- non-zero = crash/error (parent writes error entry, lets LLM recover)

No signal pipes, no named FIFOs, no custom IPC. Just: fork, wait, read DB.

### 20. Why Process over Thread for LLM

- LLM calls take 1–30+ seconds; process exits clean (all memory reclaimed)
- No leak accumulation across turns (curl, arena, jsmn buffers)
- If curl hangs: `kill(pid, SIGKILL)` — no thread cancellation mess
- Parent stays responsive (can handle signals, other agents in daemon mode)
- Memory safety: a bug in response parsing can't corrupt parent state
- Clean separation: `cclaw-llm` is a standalone testable unit

### 21. Logging: syslog with journald detection

The log collector process + journal.db are removed entirely. Logging becomes:

**Daemon mode:**
- At startup, detect journald: check `/run/systemd/journal/socket` exists
- If journald: use `sd_journal_send()` for structured fields (AGENT_NAME, SESSION_ID, PID)
- If no journald: `openlog("cclaw", LOG_PID, LOG_DAEMON)` + `syslog()`
- Parent captures child stderr via pipe → routes each line to syslog/journald
- No SQLite writes for logging — zero DB contention from log I/O

**CLI mode:**
- Child stderr piped to parent → tee to terminal stderr (already implemented)
- No syslog in CLI mode (user is watching the terminal)

**What gets removed:**
- `src/log_collector.c` (~300 LOC)
- `include/log_collector.h`
- `log` table from schema
- SCM_RIGHTS fd passing, collector fork/reap, epoll for log fds
- `journal.db` file and all references

**Target platforms:**
- systemd/journald: modern distros (AL2023, Ubuntu, Fedora) → structured logs via `sd_journal_send`
- syslog only: Pogoplug (Debian Bookworm armel, busybox syslogd), Chromebook containers → traditional syslog
- Neither: logs go to stderr only (always available as fallback)
