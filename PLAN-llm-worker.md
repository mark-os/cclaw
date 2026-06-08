# LLM Worker + Syslog + Exit Code Removal

## Problem Statement

1. Each LLM turn pays a full TLS handshake (~113ms Chromebook, ~2000ms Pogoplug) because forked children destroy the connection. A persistent worker process with thread pool reuses connections.
2. Logging is `fprintf(stderr)` with hand-rolled timestamps. Unify to `syslog()` for consistency, persistence, and cross-process correlation.
3. `LLM_EXIT_*` codes are vestigial — the parent can determine next action from DB state. Remove them to harmonize fork mode with worker mode.

## Requirements

- `--llm-fork` flag opts into old fork-per-turn behavior. Default is worker mode.
- `--llm-threads=N` sets worker thread pool size (default 2)
- Worker is a single child process with N threads, each owning a `CURL *` handle
- Parent communicates via pipes: send `session_id`, receive `session_id` on completion
- Parent queries DB for next action (pending tool_calls or not) — no exit codes
- All logging via `syslog()` with `LOG_PERROR` for CLI stderr tee
- Fork mode also uses DB-based completion detection (no exit codes)

## Architecture

```
┌───────────── Parent Process ──────────────────────────────┐
│                                                           │
│  epoll: stdin, sigchld_pipe, worker_pipe[0]               │
│                                                           │
│  on turn needed:                                          │
│    write {session_id} → worker request pipe               │
│                                                           │
│  on worker result pipe readable:                          │
│    read {session_id}                                      │
│    query DB: pending tool_calls? → dispatch               │
│              no tool_calls? → deliver_response + idle     │
│                                                           │
│  on SIGCHLD:                                              │
│    reap tool children (unchanged)                         │
│    if worker PID died → respawn worker                    │
│                                                           │
└───────────────────────────────────────────────────────────┘
        │
        │ fork once at startup
        │ pipe pair: request_pipe[2], result_pipe[2]
        ▼
┌───────────── Worker Process ──────────────────────────────┐
│                                                           │
│  setrlimit(RLIMIT_AS, 256MB)                              │
│  prctl(PR_SET_PDEATHSIG, SIGTERM)                         │
│                                                           │
│  Thread pool (N threads, default 2):                      │
│  ┌─────────────────────────────────────────────────────┐  │
│  │ thread[i]:                                          │  │
│  │   CURL *curl = curl_easy_init()  (persistent)       │  │
│  │   sqlite3 *db = db_open()        (persistent)       │  │
│  │   loop:                                             │  │
│  │     dequeue session_id from work queue              │  │
│  │     llm_turn(db, curl, session_id)                  │  │
│  │     write {session_id} → result_pipe                │  │
│  └─────────────────────────────────────────────────────┘  │
│                                                           │
│  Main thread:                                             │
│    loop:                                                  │
│      read {session_id} from request_pipe                  │
│      enqueue to work queue                                │
│                                                           │
│  stdout inherited from parent (CLI token streaming)       │
│                                                           │
└───────────────────────────────────────────────────────────┘
```

## Pipe Protocol

```
Parent → Worker:  int64_t session_id       (8 bytes)
Worker → Parent:  int64_t session_id       (8 bytes)
```

No status codes. Parent queries DB after receiving session_id back.

## Task Breakdown

### Task 1: Unify logging to syslog

**Objective:** Replace `CCLAW_LOG` macro with `syslog()`. Call `openlog()` at startup.

**Implementation:**
- Rewrite `include/log.h`: map LOG_LEVEL_ERROR→LOG_ERR, LOG_LEVEL_INFO→LOG_INFO, LOG_LEVEL_DEBUG/TRACE→LOG_DEBUG. Remove manual timestamp formatting.
- Add `openlog("cclaw", LOG_PID | LOG_PERROR, LOG_USER)` at top of `main()`, channel_runner main(), and mjs_main()
- Replace bare `fprintf(stderr, ...)` in src/ (~36 occurrences) with LOG_ERROR/LOG_INFO where appropriate. Keep `fprintf(stderr)` only for user-facing messages (prompt, usage, streaming tokens).
- `LOG_PERROR` flag ensures stderr tee for interactive CLI

**Test:** `make test` passes; verify syslog output  
**Demo:** `cclaw --log-level=debug` shows structured messages in both stderr and `journalctl -t cclaw`

### Task 2: Remove LLM_EXIT codes — DB-based completion detection

**Objective:** Replace exit-code-based dispatch with DB query after turn completion.

**Implementation:**
- New function `turn_complete(sqlite3 *db, int64_t session_id, const char *agent_name, int iteration)` in main.c:
  - `db_tool_call_get_pending()` — if count > 0: dispatch tools
  - If count == 0: check last entry's stop_reason — if error, report; else deliver_response + idle
- Update `reap_children()` CHILD_LLM_REQ handler: ignore exit code, call `turn_complete()`
- Fork-mode `llm_proc_main` return value becomes internal-only (for logging)
- Update `test_run_session.h`: use DB query instead of return code
- Update integration tests (~15 files): assert DB state instead of `LLM_EXIT_*` codes
- Keep LLM_EXIT defines temporarily as internal constants (remove in follow-up)

**Test:** All integration tests pass with DB-based assertions  
**Demo:** Fork mode works identically but without relying on exit codes

### Task 3: Refactor `llm_proc_main` into reusable `llm_turn()`

**Objective:** Extract core turn logic into a function that accepts persistent handles.

**Implementation:**
- New: `int llm_turn(sqlite3 *db, CURL *curl, int64_t session_id)`
  - Accepts existing DB connection and optional CURL handle (NULL = create/destroy per call)
  - Config load, agent setup, context plan, LLM call, ingest — all inside
  - Returns 0 success, -1 error (internal only)
- `llm_proc_main` becomes: open db → `llm_turn(db, NULL, session_id)` → close db
- Add `CURL *curl_handle` field to `HttpRequestOpts` in http.h
- In `http_do`: if `curl_handle` non-NULL, use it + `curl_easy_reset()`, skip init/cleanup

**Test:** `llm_proc_main` still works for `--llm-fork` and `cclaw llm` mode  
**Demo:** `llm_turn()` callable with or without persistent curl handle

### Task 4: Implement LLM worker process + thread pool

**Objective:** Create `src/llm_worker.c` + `include/llm_worker.h`.

**Implementation:**
- `llm_worker_start(const char *db_path, int num_threads)`: fork child, set up pipes, child applies setrlimit + prctl, spawns N threads each with own CURL + sqlite3
- Worker main thread: `while (read(request_pipe, &session_id, 8)) { enqueue }`
- Worker threads: `while (dequeue) { llm_turn(db, curl, session_id); write(result_pipe, &session_id, 8); }`
- API:
  - `llm_worker_submit(int64_t session_id)` — write to request pipe
  - `llm_worker_fd()` — return result pipe fd for epoll
  - `llm_worker_read(int64_t *session_id)` — read completed session_id
  - `llm_worker_stop()` — close pipes, kill child
  - `llm_worker_respawn()` — re-fork after crash
- Thread pool: mutex + condvar work queue
- Stdout: inherited from parent for CLI streaming; sub-agents use dup2(/dev/null)

**Test:** Start worker, submit session (CCLAW_LLM_MOCK), read completion, verify DB  
**Demo:** Worker stays alive, handles multiple turns with connection reuse

### Task 5: Integrate worker into parent event loop

**Objective:** Wire worker into main.c epoll as alternative dispatch path.

**Implementation:**
- Add `--llm-fork` and `--llm-threads=N` flag parsing
- `static int g_llm_fork` flag (default 0 = use worker)
- On startup: if !g_llm_fork, `llm_worker_start()`, add `llm_worker_fd()` to epoll
- Replace `fork_llm_req()` call sites: `if (g_llm_fork) fork_llm_req(...); else llm_worker_submit(session_id);`
- New epoll handler for worker_fd: `llm_worker_read(&session_id)` → `turn_complete()`
- Track in-flight sessions for crash recovery
- Handle worker death: SIGCHLD → mark in-flight crashed → respawn

**Test:** Multi-turn CLI conversation with worker mode  
**Demo:** Second turn measurably faster (no TLS handshake)

### Task 6: Benchmarking + verification

**Objective:** Instrument and verify TLS savings.

**Implementation:**
- Log `CURLINFO_NUM_CONNECTS` (0 = reused) and `CURLINFO_APPCONNECT_TIME` after perform
- Benchmark script: 3 consecutive turns, report per-turn TTFB
- Test on Chromebook and Pogoplug (`ssh pogoplug`)

**Test:** Second turn shows 0 new connections in debug log  
**Demo:** Pogoplug: ~2s first turn → ~0.1s subsequent turns

## What Changes, What Doesn't

| Component | Changes? |
|-----------|----------|
| `main.c` dispatch | Add worker submit path + turn_complete() |
| `main.c` epoll | Add worker_fd handler |
| `include/log.h` | Rewrite to syslog |
| `http.c` / `http_do` | Accept optional CURL* in opts |
| `llm_proc.c` | Refactor into `llm_turn()` + wrapper |
| `llm_transport.c` | No change |
| `request_stream.c` | No change |
| `db.c` | No change |
| Tool dispatch | No change (still fork-based) |
| Context building | No change |
| SSE parsing | No change |
| Integration tests | Assert DB state instead of exit codes |

## Flags

```
cclaw                    # default: worker mode, 2 threads
cclaw --llm-fork         # old behavior: fork per turn
cclaw --llm-threads=4    # worker with 4 threads
```
