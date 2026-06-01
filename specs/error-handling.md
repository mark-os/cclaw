# Error Handling & Recovery

LLM API calls fail in many ways. This doc classifies failure modes, defines recovery behavior, and specifies where/how errors surface.

## Principles

- Agent turn always ends w/ DB write (entry or error entry)
- `get_response_text(db, session_id)` resolves deliverable text from branch — sole source of truth for all channels
- Retry ! ⊥ mutate session state — re-stream same request from DB
- Errors that exhaust retries → write `stop_reason=error` entry → agent exits 0 (turn complete, error is the response)
- Channel delivery reads final state from DB post-exit — never from in-flight memory

## Error Classification Table

| id | category | detection | retry? | max attempts | fallback? | entry written? | user message |
|----|----------|-----------|--------|--------------|-----------|----------------|--------------|
| E1 | zero-usage empty stop | HTTP 200 + `usage.total_tokens == 0` + content null/empty + `finish_reason == "stop"` | yes | 2x primary + 1x fallback | yes | only on final failure | "model returned empty response — provider glitch, try again" |
| E2 | HTTP 429 rate limit | status == 429 | yes | 3x w/ backoff (respect `Retry-After`) | no (same provider) | only on exhaust | "rate limited by provider, retrying..." |
| E3 | HTTP 5xx server error | status ∈ [500,599] | yes | 3x w/ backoff | yes (fallback chain) | only on exhaust | "provider server error" |
| E4 | network failure | status == -1 (curl error) | no | — | yes (fallback chain) | on exhaust | "network error: could not reach provider" |
| E5 | context overflow | HTTP 400 + error text contains "context" / "too long" / "maximum" | no | — | no | yes (error entry) | agent returns -2, caller handles (compaction or truncation) |
| E6 | JSON parse failure | valid HTTP 200 but body ⊥ valid JSON | yes | 3x | no | only on exhaust | "malformed response from provider" |
| E7 | missing finish_reason | valid JSON, choices present, but no `finish_reason` | yes | 3x | no | only on exhaust | "incomplete response from provider" |
| E8 | token limit exhaustion | `finish_reason == "length"` | no | — | no | yes (partial content preserved) | (deliver partial content as-is) |
| E9 | content filter | `finish_reason == "content_filter"` | no | — | no | yes (stop_reason=error) | "response filtered by provider safety policy" |
| E10 | timeout | curl timeout (CURLE_OPERATION_TIMEDOUT) | no | — | yes (fallback chain) | on exhaust | "request timed out" |
| E11 | auth failure | HTTP 401/403 | no | — | yes (fallback chain) | on exhaust | "authentication failed — check API key" |
| E12 | model not found | HTTP 404 / error mentions model | no | — | yes (fallback chain) | on exhaust | "model not available" |

## Zero-Usage Retry Flow (E1)

```
agent_run loop iteration:
  1. context_plan() → entry IDs
  2. llm_call_with_fallback_stream() → HTTP 200
  3. llm_parse_response() → success
  4. CHECK: usage.total_tokens == 0 && content null/empty && finish_reason == "stop"
     → if yes AND retries_remaining > 0:
        - ⊥ write entry to DB
        - ⊥ manipulate session state
        - decrement retry counter
        - continue (re-plan, re-stream same context)
     → if yes AND retries_remaining == 0 AND fallback configured:
        - try 1x with fallback model
        - if fallback also fails → write error entry
     → if no: proceed normally
```

Key: `rs_reset()` allows re-streaming same request body. No inbox drain, no state change. Pure retry.

## Response Resolution

`get_response_text(db, session_id)`:
1. Walk branch backward from leaf
2. Return first assistant entry w/ non-empty content
3. Stop at user boundary (⊥ leak previous turn)
4. Return NULL if nothing found → channel delivers nothing (silent failure)

Note: OpenClaw concatenates ALL non-empty assistant texts from turn as fallback; we return last non-empty only. Revisit if needed.

## Entry Write Rules

| scenario | write entry? | stop_reason | content |
|----------|-------------|-------------|---------|
| normal response | yes | stop | LLM content |
| tool_calls response | yes | tool_use | NULL (tool_calls in separate column) |
| partial (length) | yes | stop (length mapped) | partial content preserved |
| error after retries | yes | error | error description for user |
| zero-usage retry (not exhausted) | NO | — | — |
| shutdown signal | yes | aborted | "error: agent terminated by shutdown signal" |
| content filter | yes | error | filter message |

## Async Considerations

Agent process lifecycle:
1. Fork → drain inbox → LLM loop → write final entry → exit code
2. Parent (daemon/CLI) reaps → reads DB → delivers response

The agent ! always write a final entry before exit. The delivery layer reads from DB post-exit. There is no in-flight state to lose — everything is persisted before the process dies.

Exception: zero-usage retry happens WITHIN the agent process (no exit/re-fork). The agent simply loops internally. From the parent's perspective, the turn just takes longer.

## Log Levels & Observability

| level | what's logged | destination |
|-------|---------------|-------------|
| info (default) | turn start/end, errors, warnings | stderr → journal.db |
| debug | + tool dispatch, LLM timing, context plan stats, retry decisions | stderr → journal.db |
| trace | + full req/resp JSON (current `--debug` behavior) | stderr → journal.db |

`CCLAW_LOG_LEVEL` env var, stored in cclaw.db kv as `log_level`, injected at fork. Both CLI and daemon pipe child stderr to journal.db (CLI also tees to terminal).

## CLI/Daemon Logging Parity

Current state:
- Daemon: pipe(stdout/stderr) → log_collector → journal.db
- CLI: child inherits terminal (no journal)

Target state:
- Both: pipe(stderr) → parent reads → journal.db + (CLI: echo to terminal)
- stdout: same treatment (tee for CLI, pipe-only for daemon)

Implementation: CLI `cli_fork_turn` creates pipes like daemon does. Parent thread/loop drains pipe, writes to journal.db, echoes to terminal. `waitpid` after pipe EOF.
