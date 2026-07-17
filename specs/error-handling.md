# Error Handling & Recovery

LLM API calls fail in many ways. This doc classifies failure modes, defines recovery behavior, and specifies where/how errors surface.

## Principles

- Agent turn always ends w/ DB write (entry or error entry)
- `get_response_text(db, session_id)` resolves deliverable text from branch — sole source of truth for all channels
- Retry ! ⊥ mutate session state — re-stream same request from DB
- Errors that exhaust retries → write `stop_reason=error` entry → agent exits 0 (turn complete, error is the response)
- Channel delivery reads final state from DB post-exit — never from in-flight memory

## Error Classification Table

The whole policy lives in one loop in `llm_req()` (`src/llm_proc.c`). Three
outcomes exist:

- **transient** — backoff retry on the same model (1s/2s/4s, `Retry-After`
  honored), then fall through to the next candidate in the routing order.
- **permanent for this model** — no resend can change the answer; skip to the
  next candidate immediately.
- **our-side fatal** — abort the turn; paying another provider won't fix our DB.

| id | category | detection | retry same model? | next model? | counts toward degrade? | user message |
|----|----------|-----------|-------------------|-------------|------------------------|--------------|
| E1 | empty completion | HTTP 2xx + zero usage + content null/empty + `finish_reason == "stop"` (also: 2xx with empty body) | yes, 3x w/ backoff | yes | yes | "provider returned an empty response" |
| E2 | HTTP 429 rate limit | status == 429 | yes, 3x w/ backoff (respect `Retry-After`) | yes | yes (429 threshold) | "rate limited" |
| E3 | HTTP 5xx server error | status in [500,599] | yes, 3x w/ backoff | yes | yes | "provider server error" |
| E4 | network failure | status == -1 (curl error) | yes, 3x w/ backoff | yes | yes | "network error" |
| E5 | context overflow | HTTP 400 + `llm_is_context_overflow()` | no | yes (a bigger-window model may fit) | no (prompt-specific) | "prompt too large for the model's context window" |
| E6 | malformed body | 2xx but body not valid JSON, or no/empty choices | yes, 3x w/ backoff | yes | yes | "provider returned a malformed response" |
| E8 | token limit exhaustion | `finish_reason == "length"` | no | — | no | (deliver partial content as-is) |
| E9 | content filter | `finish_reason == "content_filter"` | no | — | no | (stop_reason mapped; content delivered) |
| E10 | timeout | status == -2 (curl timeout) | yes, 1x (attempts cost minutes; no sleep between) | yes | yes | "request timed out" |
| E11 | auth failure | HTTP 401/403 | no | yes + degrade w/ cooldown | — | "authentication failed" |
| E12 | model not found | HTTP 404 | no | yes + degrade w/ cooldown | — | "model not available" |
| E13 | other 4xx | remaining non-2xx | no | yes | no | "provider rejected the request" |
| E14 | DB error during ingest | SQLITE_BUSY on our side | no | **no — turn aborts** | no | "DB contention during response ingest (SQLITE_BUSY) — check logs" |

Every failed attempt is archived to `llm_responses` (response body + the request
we sent). The final error entry cites the last archived row as `[resp #N]`;
read it with `cclaw resp <N>` (or `cclaw resp <N> req` for the request). With a
single-model route there is no fallback — after same-model retries exhaust, the
turn fails with the message above.

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
| transient-failure retry (not exhausted) | NO | — | — |
| shutdown signal | yes | aborted | "error: agent terminated by shutdown signal" |
| content filter | yes | error | filter message |

## Async Considerations

Agent turn lifecycle:
1. Worker thread: llm_req() → write entry to DB → notify main
2. Main thread: advance_session() → dispatch tools or deliver response

The agent always writes a final entry before completion. The delivery layer reads from DB. There is no in-flight state to lose — everything is persisted before advancing.

Exception: transient-failure retries (E1–E4, E6, E10) happen WITHIN llm_req (no re-dispatch). The worker simply retries internally. From the main loop's perspective, the turn just takes longer.

## Log Levels & Observability

Implemented via `LOG_*` macros in `include/log.h`. All output → stderr → pipe → parent.

| level | env value | what's logged | destination |
|-------|-----------|---------------|-------------|
| error | `error` | errors only | stderr → syslog/terminal |
| info (default) | `info` | + turn start/end, warnings | stderr → syslog/terminal |
| debug | `debug` | + tool dispatch, LLM timing, context plan stats, retry decisions, response shapes | stderr → syslog/terminal |
| trace | `trace` | + full req/resp JSON | stderr → syslog/terminal |

`CCLAW_LOG_LEVEL` env var, injected at fork (no DB key).
CLI `--log-level=trace` enables full req/resp JSON. CLI `-v`/`--debug` tees stderr pipe to terminal.

Format: `HH:MM:SS.mmm [LEVEL] message\n` — parseable by syslog and grep.

## CLI/Daemon Logging Parity

**Daemon mode:**
- `pipe(stderr)` → parent drains → syslog (`LOG_DAEMON` facility; `sd_journal_send` if journald available)
- `pipe(stdout)` → /dev/null (response delivered from DB post-exit)

**CLI mode:**
- `pipe(stderr)` → parent drains → tee to terminal (if log_level ≥ debug, e.g. `-v`/`--debug`)
- stdout: inherited by child (goes directly to terminal for streaming display)

**Design principle:** stderr = structured logs (daemon persists via syslog). stdout = user content (display only).
