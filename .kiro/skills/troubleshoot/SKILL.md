---
name: troubleshoot
description: CClaw runtime investigation. Triggers on "why did the agent fail", "blank response", "debug session", "what happened", "troubleshoot", "investigate turn", "check logs". Walks through data sources to diagnose LLM API failures, provider glitches, and session corruption.
---

# troubleshoot — CClaw runtime investigation

Triggers: "why did the agent fail", "blank response", "debug session", "what happened", "troubleshoot", "investigate turn", "check logs"

## Investigation Steps

### 1. Check the session entries

```sql
-- Get entries for the problematic session (replace N)
SELECT id, parent_id, role, substr(content, 1, 100) as preview,
       tool_calls IS NOT NULL as has_tc, tool_call_id, stop_reason,
       usage_in, usage_out, turn_id, data
FROM entries WHERE session_id=N ORDER BY id;
```

Role mapping: 0=system, 1=user, 2=assistant, 3=tool, 4=compaction

Stop reason mapping: 0=none, 1=stop, 2=length, 3=tool_use, 4=error, 5=aborted

### 2. Check for zero-usage (provider glitch)

```sql
-- Find assistant entries with no token usage (provider returned empty)
SELECT id, session_id, turn_id, content, usage_in, usage_out
FROM entries WHERE role=2 AND usage_in IS NULL AND usage_out IS NULL
  AND stop_reason=1 ORDER BY id DESC LIMIT 10;
```

If found: provider returned HTTP 200 with 0 tokens. See `specs/error-handling.md` E1.

### 3. Check stderr logs

Daemon mode: logs go to syslog (`journalctl -u cclaw` or `/var/log/syslog`).
CLI mode: stderr tees to terminal. Use `--log-level=trace` for full details.

```bash
# Recent syslog entries (if using systemd)
journalctl -u cclaw --since "5 min ago"
```

### 4. Reproduce with --log-level=trace

```bash
echo "your prompt" | ./build/cclaw --log-level=trace --new 2>debug.txt
```

`debug.txt` contains `[DEBUG REQ]` (full request JSON) and `[DEBUG RESP]` (status + response JSON). Check:
- Was the request well-formed?
- Did the response have `usage.total_tokens > 0`?
- What `provider` field was in the response?
- Was `content` null/empty?

### 5. Check provider routing

The `provider` field in OpenRouter responses shows which backend handled it. Known problematic:
- **GMICloud**: intermittent zero-usage empty responses on tool-result follow-ups (B1)

### 6. Reproduce with curl

Use `scripts/repro_empty_response.sh` to test provider reliability in isolation (no CClaw code path).

### 7. Check response resolution

`get_response_text(db, session_id)` walks branch backward from leaf:
1. Finds last assistant with non-empty content
2. Stops at user boundary
3. Returns NULL if nothing found

If user saw blank output but DB has content → check `leaf_id` in sessions table matches expected entry.

### 8. Check session state

```sql
SELECT id, name, leaf_id, state FROM sessions WHERE id=N;
```

- `state=idle` after turn → normal
- `state=running` → agent crashed mid-turn (V30)
- `state=waiting` → daemon action pending (spawn/approval/config)

## Common Failure Patterns

| symptom | likely cause | check |
|---------|-------------|-------|
| blank response | zero-usage provider glitch (E1) | step 2 |
| blank response | empty content stored as `""` | entries table, content column |
| "error: LLM request failed after retries" | HTTP 429/5xx exhausted retries | journal stderr, --log-level=trace |
| no response at all | agent crashed (SIGKILL/OOM) | session state=running, no final entry |
| hangs forever | worker thread deadlock/crash | step 9 |
| partial response | `finish_reason=length` (E8) | stop_reason=2 in entries |
| wrong response shown | stale leaf_id (WAL visibility) | compare leaf_id vs max entry id |

## 9. Process-level hangs and crashes

When cclaw hangs (no output, no error, prompt never returns), the issue is
below the LLM layer — in the worker threads, DB, or process plumbing.

### Triage order

```bash
# 0. Kill orphan cclaw processes from previous runs
pkill -9 -f "build/cclaw"; sleep 1

# 1. Reproduce with trace — redirect to FILE, never pipe through head/grep
timeout 10 ./build/cclaw --log-level=trace --new -p "test" >/tmp/out.txt 2>/tmp/err.txt
# Then read the files:
cat /tmp/err.txt

# 2. Check if it's the worker or the LLM path — run LLM directly
sqlite3 ~/.cclaw/cclaw.db "SELECT id FROM sessions ORDER BY id DESC LIMIT 1;"
CCLAW_DB=~/.cclaw/cclaw.db timeout 10 ./build/cclaw llm -s <ID>
# If this works instantly → problem is worker subsystem, not LLM

# 3. Try fork mode (bypasses worker threads entirely)
timeout 10 ./build/cclaw --llm-fork --new -p "test"
# If this works → problem is specifically in the worker thread pool

# 4. For crashes/segfaults — use the debug build
make debug
timeout 10 ./build/cclaw --new -p "test"
# ASAN prints stack trace on crash. No strace/gdb needed.

# 5. For deadlocks — check what the process is doing
./build/cclaw --new -p "test" & PID=$!; sleep 3
ls -la /proc/$PID/fd/          # file descriptors
cat /proc/$PID/stack           # kernel stack (shows syscall)
kill $PID
```

### Rules for debugging agents

- **Never pipe cclaw output through head/grep/tail** — SIGPIPE kills the
  process and gives false results. Always redirect to file first, then read.
- **Kill orphans first** — stale processes hold DB locks and confuse pgrep.
- **Use `make debug` for crashes** — ASAN catches NULL derefs, UAF, buffer
  overflows with exact stack traces. Faster than strace.
- **`--llm-fork` isolates** — if fork mode works and worker mode doesn't,
  the bug is in the thread pool, not the LLM code path.
- **`./build/cclaw llm -s <id>` isolates further** — runs one LLM call in a
  clean subprocess. If this works, the request building and HTTP are fine.

### Worker thread observability

The worker threads log to syslog (LOG_PERROR tees to stderr):
- `worker: session=N model start` — thread picked up job
- `llm_req: Nms status=200 model=X` — HTTP call completed
- `worker: config_load failed` — DB or schema issue

At `--log-level=trace`, `sqlite3_trace_v2` logs any query taking >1ms.

### Known past issues (resolved)

| issue | root cause | fix |
|-------|-----------|-----|
| worker thread deadlock | bare fork (no exec) inherited poisoned glibc mutexes | switched to in-process threads |
| config_load returns NULL base_url | providers table query fails on stale schema, fallback was inside the if-guard | moved fallback outside prepare-success block |
| dispatcher drain loop blocks | pipe fd not set to O_NONBLOCK after exec | set O_NONBLOCK on ping fd |

## Files

- `specs/error-handling.md` — full error classification table
- `scripts/repro_empty_response.sh` — provider reliability test
- DB: `~/.cclaw/cclaw.db` (all state: sessions, entries, config)
