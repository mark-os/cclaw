# Error Handling & Recovery

LLM API calls fail in many ways. This doc classifies failure modes, defines recovery behavior, and specifies where/how errors surface.

## Principles

- Agent turn always ends w/ DB write (entry or error entry)
- `get_response_text(db, session_id)` resolves deliverable text from branch — sole source of truth for all channels
- Retry ! ⊥ mutate session state — re-stream same request from DB
- Errors that exhaust retries → write `stop_reason=error` entry → agent exits 0 (turn complete, error is the response)
- Channel delivery reads final state from DB post-exit — never from in-flight memory

## Error Classification Table

The whole policy lives in one selection loop in `llm_req()` (`src/llm_proc.c`),
and **degradation IS the retry policy** (plan/projects/model-routing.md R3):
each pass picks the first candidate in the agent's routing list
(`agent_models`, pos order) that isn't skipped for this request, isn't inside
an error cooldown, and hasn't hit `health_fail_threshold` attempts locally. A
model keeps getting attempts only while it's the best eligible candidate; its
own `consec_failures` crossing the threshold (default 4, any transient error
counts, any success resets) is what dethrones it — the cooldown stamp is
re-applied on EVERY crossing, so a still-dead model keeps getting re-sidelined
after its cooldown lapses. When every candidate is cooling down, the first one
still gets ONE live attempt per iteration (health reorders the list, never
empties it — and that attempt doubles as the early-recovery probe).

Backoff is 1s/2s/4s between consecutive attempts on the SAME model; switching
models sleeps zero. `Retry-After` ≤ 4s replaces the scheduled backoff; a
longer one degrades the model immediately with
`degraded_until = now + max(Retry-After, health_cooldown_sec)`.

Three outcome classes exist:

- **transient** — count toward `consec_failures` and loop (selection decides
  who's next).
- **permanent for this model** — no resend can change the answer; locally skip
  the candidate (E11/E12/other-400 also degrade with a cooldown).
- **our-side fatal** — abort the turn; paying another provider won't fix our DB.

| id | category | detection | class | counts toward degrade? | user message |
|----|----------|-----------|-------|------------------------|--------------|
| E1 | empty completion | HTTP 2xx + zero usage + content null/empty + `finish_reason == "stop"` (also: 2xx with empty body) | transient | yes | "provider returned an empty response" |
| E2 | HTTP 429 rate limit | status == 429 | transient (`Retry-After` > 4s = immediate degrade) | yes | "rate limited" |
| E3 | HTTP 5xx server error | status in [500,599] | transient | yes | "provider server error" |
| E4 | network failure | status == -1 (curl error) | transient | yes | "network error" |
| E5 | context overflow | HTTP 400 + `llm_is_context_overflow()` | skip candidate (a bigger-window model may fit) | no (prompt-specific) | "prompt too large for the model's context window" |
| E6 | malformed body | 2xx but body not valid JSON, or no/empty choices | transient | yes | "provider returned a malformed response" |
| E8 | token limit exhaustion | `finish_reason == "length"` | terminal | no | (deliver partial content as-is) |
| E9 | content filter | `finish_reason == "content_filter"` | terminal | no | (stop_reason mapped; content delivered) |
| E10 | timeout | status == -2 (curl timeout) | transient | yes | "request timed out" |
| E11 | auth failure | HTTP 401/403 | skip + degrade w/ cooldown | — | "authentication failed" |
| E12 | model not found | HTTP 404 | skip + degrade w/ cooldown | — | "model not available" |
| E13 | other 4xx | remaining non-2xx | skip candidate (400: also degrade) | no | "provider rejected the request" |
| E14 | DB error during ingest | SQLITE_BUSY on our side | **turn aborts** | no | "DB contention during response ingest (SQLITE_BUSY) — check logs" |

**Operator messaging** (model-routing.md R6): there is no per-degrade notice.
After each success, the model that served is compared to the agent's previous
success (from `llm_responses`); on any change one notice names the new server
and the cause (knob: `notify_model_change`, default on). All-models-down error
deliveries to a channel are deduped per session — the first speaks, identical
repeats are suppressed until a human message arrives or routing recovers
(`channel_push`, advance.c).

Two failures never reach this table because they happen *before* a request
exists, and both are recorded anyway:

- **Candidate dropped for a missing key** — the model's provider names an
  `api_key_env` that resolves to neither an env var nor a system secret. WARN,
  plus `status='degraded'` with a NULL `degraded_until` (degraded by
  configuration, one operator notice on the transition, restored to `healthy`
  when the key resolves). See [providers.md](providers.md).
- **Apply-time probe failure** — a config change that moves routing is proved
  against the real request path before it stands; a non-2xx, transport error or
  15s timeout reverts the change and says so. Archived as `probe_*` in
  `llm_responses`, and never counted as model degradation (a probe is a config
  event, not traffic). See [self-configuration.md](self-configuration.md).

Every failed attempt is archived to `llm_responses` (response body + the request
we sent). The final error entry cites the last archived row as `[resp #N]`;
read it with `cclaw resp <N>` (or `cclaw resp <N> req` for the request). With a
single-model route there is no fallback — after the model degrades (plus its
one all-degraded desperation attempt), the turn fails with the message above.

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

## Tool failure is an explicit status

A tool result's failure is carried by `entries.is_error`, and that flag comes
from the tool itself — never from reading the message. Handlers (and the
EXEC_THREAD and sandbox-tier leaves) take an `int *is_error` out-param and set
it at the failure site, usually via `tool_fail()`; sandboxed `--run-tool`
children frame the same verdict as a status byte ahead of the result body
(`[status][meta_len][meta][body]`), so it crosses the sandbox boundary
atomically with the bytes it describes. A child's exit code keeps its
process-lifecycle meaning (sandbox refusal, crash, signal) and never encodes
tool failure; a child that dies without answering is a failure by virtue of the
death.

Result TEXT is free-form. Most failures still read `error: ...` because that
is a clear thing to say to a model, but **nothing parses it** — a refusal
phrased any other way is just as much a failure, and a successful result that
happens to quote the word is not one. (This replaced a `strncmp(result,
"error:", 6)` sniff in three readers, which silently mis-recorded both.)

**Signal attribution.** When a sandboxed workload dies by signal, the broker
makes one attribution attempt from what it already holds — the signal number
plus the rlimits that call ran under. SIGKILL with limits configured appends
"likely resource limit … reduce usage, don't just retry"; SIGXCPU names the CPU
cap. No dmesg parsing, no probe run.

## Async Considerations

Agent turn lifecycle:
1. Worker thread: llm_req() → write entry to DB → notify main
2. Main thread: advance_session() → dispatch tools or deliver response

The agent always writes a final entry before completion. The delivery layer reads from DB. There is no in-flight state to lose — everything is persisted before advancing.

Exception: transient-failure attempts (E1–E4, E6, E10) happen WITHIN llm_req (no re-dispatch). The selection loop simply moves through the agent's list internally. From the main loop's perspective, the turn just takes longer.

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
