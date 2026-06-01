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

### 3. Check journal.db for stderr output

```sql
-- Recent log lines for a session
SELECT source, pid, stream, line, datetime(created_at, 'unixepoch') as ts
FROM log WHERE session_id=N ORDER BY id DESC LIMIT 50;
```

Stream: 1=stdout, 2=stderr. Note: CLI mode currently does NOT pipe to journal (T233 pending).

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
| partial response | `finish_reason=length` (E8) | stop_reason=2 in entries |
| wrong response shown | stale leaf_id (WAL visibility) | compare leaf_id vs max entry id |

## Files

- `specs/error-handling.md` — full error classification table
- `scripts/repro_empty_response.sh` — provider reliability test
- Agent DB: `~/.cclaw/agents/<name>/agent.db` (or `.cclaw/agents/<name>/agent.db` relative)
- Journal: `~/.cclaw/journal.db` (daemon mode only until T233)
