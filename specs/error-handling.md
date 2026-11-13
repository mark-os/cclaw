# Error Handling & Recovery

LLM API calls fail in many ways. This doc classifies failure modes, defines recovery behavior, and specifies where/how errors surface.

## Principles

- Every LLM request ends with a DB write: an assistant entry or an error entry.
- A retry does not change session state. It resends the same request, rebuilt from the DB.
- When the selection loop has no candidate left, `llm_req()` writes an entry with `stop_reason=error`. The turn is then complete, and the error text is its response.
- Delivery reads the final state from the DB, never from in-flight memory. `get_response_text(db, session_id)` resolves the final-message text from the branch (see Response Resolution).

## Error Classification Table

The whole policy lives in one selection loop in `llm_req()` (`src/llm_proc.c`),
and **degradation IS the retry policy**:
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
| E7 | tool call as unparsed markup | 2xx + content null/empty + no `tool_calls` + `reasoning` contains tool-call markup (`<｜DSML｜…`, `<|tool_call…`) | transient | yes | "provider returned a tool call as unparsed markup" |
| E8 | token limit exhaustion | `finish_reason == "length"` | terminal | no | (deliver partial content as-is) |
| E9 | content filter | `finish_reason == "content_filter"` | terminal | no | (stop_reason mapped; content delivered) |
| E10 | timeout | status == -2 (curl timeout) | transient | yes | "request timed out" |
| E11 | auth failure | HTTP 401/403 | skip + degrade w/ cooldown | — | "authentication failed" |
| E12 | model not found | HTTP 404 | skip + degrade w/ cooldown | — | "model not available" |
| E13 | other 4xx | remaining non-2xx | skip candidate (400: also degrade) | no | "provider rejected the request" |
| E14 | DB error during ingest | SQLITE_BUSY on our side | **turn aborts** | no | "DB contention during response ingest (SQLITE_BUSY) — check logs" |

### Operator notices

A system notice is a message the daemon writes itself, about the system, for
the person who runs it. There are four: the model serving an agent changed,
a session's compaction keeps failing, a cron job was auto-paused, and a
session hit its autonomous-turn limit. `channel_notify_operator()` sends
each one.

A system notice goes to the channel's **operator destination** and names the
agent and session. It does not go to the session's chat: that chat can be a
group, and its members are not the operator.

The operator destination of a channel is (`channel_operator_queue()`):

1. the chats listed in `<channel>.admin_chats`, when the list is not empty;
2. otherwise a direct message to each user in `<channel>.admin_ids`;
3. otherwise, for a system notice, the session's own chat (a plain 1:1 setup
   needs no config). An error is not sent to the session's chat this way; see
   below.

`admin_chats` designates a chat, usually a private group, as the place the
operator reads. It replaces the direct messages; it does not add to them. It
gives no authority: admin commands and approval decisions still need a sender
in `admin_ids`, and approval prompts still go to each admin directly. The
"unknown sender" and "channel reverted" notices use the same destination. A
session with no channel gets no notice.

The "message deferred" reply to a rate-limited or over-budget request is not a
system notice. It answers a message, and it goes to the session's chat.

### A turn that ends in an error

The error text is for the operator. No other reader gets it. The one
distinction is who reads the chat, not what kind of chat it is.

An **admin chat** is a chat the operator reads and nobody else should need
to: a chat listed in `admin_chats`, or a direct message whose other party is
one of the channel's `admin_ids`. Every other chat is a **non-admin chat**: a
group that is not listed (even with an admin in it), and a direct message with
anyone else.

`error_notify()` (advance.c) applies the rule to every failed turn: an LLM
failure, a failed cron script, and a failed turn on an `explicit` chat.

| The chat is | The error text goes to | The chat gets |
|-------------|------------------------|---------------|
| an admin chat | the operator destination (often this same chat) | nothing more |
| a non-admin chat | the operator destination | the generic line |

If the channel has neither `admin_chats` nor `admin_ids`, there is no operator
to send the error to. The error stays in the log and in `entries`, a WARN line
says so, and the chat still gets the generic line.

The generic line is `chat_error_text` (default "Sorry, I can't provide a
response right now."). A chat gets it at most once per
`chat_error_notice_hours` (default 24; 0 = never). The limit is per chat, not
per session: `sessions.chat_error_notice_at` holds the time, and the check
takes the newest value over all sessions of the chat.

`sessions.chat_is_admin` records whether the chat is an admin's own DM. The
daemon sets it from each incoming message (`chat_type` is `dm` and `sender_id`
is an admin). Membership of `admin_chats` is read from config each time.
Until a message has set it, a chat whose id is itself an admin id counts as an
admin chat (a Telegram DM); any other chat does not.

Repeats are suppressed. If the newest outbox row of the session is the same
error (compared up to the `[resp #N]` citation) and no human message arrived
after it, nothing is sent. This stops a cron job from repeating the notice on
every fire during an outage.

Before v55 an LLM failure sent nothing to any chat, and the other errors went
to the session's chat as text.

There is no per-degrade notice.
After each success, the model that served is compared to the agent's previous
success (from `llm_responses`); on any change one notice names the new server
and the cause (knob: `notify_model_change`, default on).

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
1. Walk the branch backward from the leaf.
2. Return the first assistant entry with non-empty content (not `tool_call` or `reasoning`).
3. Stop at the first user entry, so text from a previous turn never leaks.
4. Return NULL if nothing is found. The channel then delivers nothing.

The `digest` delivery policy concatenates assistant prose instead; see [delivery.md](delivery.md).

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

Iteration lifecycle:
1. Worker thread: `llm_req()` writes the response entry to the DB and notifies the main thread through the worker pipe.
2. Main thread: `advance_session()` dispatches tools or delivers the response.

Everything is persisted before the session advances, so there is no in-flight state to lose.

Transient-failure attempts (E1–E4, E6, E7, E10) happen inside `llm_req()`, with no re-dispatch. The selection loop moves through the agent's list internally. For the main loop, the iteration only takes longer.

## Log Levels & Observability

Implemented by the `LOG_*` macros in `src/log.h` (`src/log.c`). Every message
goes to syslog (`openlog("cclaw", LOG_PID, LOG_USER)`), with a logfmt suffix
from the thread-local context (`session=`, `turn=`, `agent=`). There is no
libsystemd dependency; journald receives the messages through syslog.

| level | value | what is logged |
|-------|-------|----------------|
| error | `error` | errors only |
| warn | `warn` | + warnings |
| info (default) | `info` | + turn start/end |
| debug | `debug` | + tool dispatch, LLM timing, context plan stats, retry decisions, response shapes |
| trace | `trace` | + full request/response JSON (tagged `trace=1`) |

The level comes from `CCLAW_LOG_LEVEL`, or from the CLI flags `-v`/`--debug`
(debug), `-vv`/`--trace` (trace) and `--log-level=LEVEL`. A flag overrides the
env var and is exported to child processes through `CCLAW_LOG_LEVEL`. With
neither set, the default is `info`, except in the interactive CLI on a
terminal, where it is `error` so that log lines do not interrupt the
conversation.

**Destinations.** The interactive CLI (not `--daemon`, not `--channel`) also
writes each message to stderr as `cclaw[<pid>]: <message>`. The daemon and
channel runners write to syslog only. A sandboxed `--run-tool` child writes to
syslog only. Stdout carries user content only.
