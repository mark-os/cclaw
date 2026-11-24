# Hooks

A hook is a script an extension ships that the runtime runs when an event
happens. It is forked, in the agent's sandbox, exactly like a scheduled
script: no extension JavaScript runs inside the daemon. Four events fire:
`Stop`, `SubagentStop`, `ContextThreshold` and `CacheExpiring`. A reply may
edit the stored session on every one of them.

Code: `src/hook.c` (the events, which hooks an agent runs, the failure
count), `src/settle.c` (the order, the hold, the replies),
`src/cache_expiring.c` (the `CacheExpiring` timer), `src/dispatch.c`
(`hook_script_spawn`, the fork and the bytes). What a script is given and
what it may return is summarised [below](#input-and-reply) and defined in
full in [turn-edit.md](turn-edit.md); the mechanism here depends on it only
through `turn_edit_input` (to build `args`), `turn_edit_apply` (to apply the
reply) and `turn_edit_measure` (the size the context events compare).

## Events

| Event | Fires | A reply may | On failure |
|---|---|---|---|
| `Stop` | a turn of a top-level session ended with an answer (not an error stop), after the reply is delivered | edit the session | nothing happened; the order goes on |
| `SubagentStop` | the same, in a sub-agent's session (`parent_session_id` > 0) | edit the session | nothing happened; the order goes on |
| `ContextThreshold` | at the end of an iteration with no call in flight, mid-turn or at turn end, when the session is over its `trim_threshold`; every session | edit the session, including `summarise`; or nothing | nothing happened; C's trim runs if the session is still over |
| `CacheExpiring` | an idle session above its `prune_target` whose prompt cache is about to lapse ([below](#cacheexpiring)); every session, daemon only | edit the session, including `summarise`; or nothing | nothing happened; nothing follows |

`PreToolUse` and `PostToolUse` are known names that do not fire yet:
promote refuses them by name, and refuses `matcher`, which belongs to them.

The thresholds come from the agent's governing routing entry — its first
chat model that is not `disabled` (`context_limits`, `src/context.c`):
`agent_models.trim_threshold` (empty = half the model's `context_window`),
`prune_target` (empty = half the threshold) and `cache_lifetime` (seconds;
empty = no `CacheExpiring`). See [memory.md](memory.md#context-window-management).

### Why mainstream names

Claude Code's hook model is the one other harnesses copy: named moments, a
process per hook, a small reply vocabulary, a failure rule set by the event.
Where a moment is the same, cclaw uses its names (`Stop`, `SubagentStop`,
`PreToolUse`, `PostToolUse`) and its manifest words (`event`, `matcher`,
`handler`, `async`, `timeout`). Two things are cclaw's own. A reply may edit
the stored session, which no other harness offers. And the two context
events: elsewhere the harness compacts and a `PreCompact` hook customises or
vetoes it; here the script *is* the compaction and C acts only when it
fails, so a "before compaction" name would describe a moment cclaw does not
have. The two events exist because their failure rules differ: over the
threshold C must trim, while on an expiring cache doing nothing is safe.

## Registering one

A `hooks[]` entry in the extension manifest ([extensions.md](extensions.md)):

```json
"hooks": [ { "event": "ContextThreshold", "handler": "compact.qjs",
             "input": { "history": "all", "max_bytes": 2097152 },
             "timeout": 600 } ]
```

| Field | Values | Meaning |
|---|---|---|
| `event` | `Stop`, `SubagentStop`, `ContextThreshold`, `CacheExpiring` | when it runs, and so what its reply may do |
| `handler` | a `.qjs` file in the bundle | the script |
| `input` | object, optional | what the script is sent besides the newest turn: `history`, `history_results`, `max_bytes` ([turn-edit.md](turn-edit.md#input)) |
| `async` | boolean, optional, default `false` | `true`: the session does not wait, and the reply is discarded ([Not waited for](#not-waited-for)) |
| `timeout` | integer seconds, 1–3600, optional | this hook's deadline; absent = config `hook_timeout` |

`extension_manifest_validate` refuses anything else, naming it: an entry
that is not an object, an unknown key, an event that does not fire,
`matcher`, a non-boolean `async`, a `timeout` out of range. A manifest that
still says `wait` is told that `"async": true` replaced it.

Promote writes one `hooks` row per entry ([schema.md](schema.md#hooks)):
`(extension_name, event, path, enabled, async, fail_count, input, pos,
timeout, disabled)`, `path` inside the shared store, `pos` the entry's index
in `hooks[]`, `input` and `timeout` as declared or NULL. Re-promoting
replaces the rows, so the failure count starts over and a hook disabled by
its failures is enabled again. The approval lists each hook on a line of its
own, saying when it runs, whether the session waits for it and, when it
does, everything the reply may change — "rewrite any message (yours or the
agent's) and the arguments of any tool call, drop tool calls and replace
their results, add notes, and drop whole turns or replace them with a summary
it writes (the originals are kept, out of the agent's sight)" — its timeout,
and what it is sent ("It is sent the newest turn and the whole conversation
before it, at most 2097152 bytes"). See
[self-configuration.md](self-configuration.md) for the whole prompt.

**Which hooks an agent runs** is read from the tables each time
(`hook_next`), never cached: `hooks ⨝ agent_extensions ⨝ extensions` where
the extension is attached to the agent and enabled there, the extension is
enabled and published or owned by that agent — the rule its tools load by —
and the hook has `enabled = 1` and `disabled = 0`. An extension attached a
moment ago is found at the next event without a restart.

**Order.** Hooks for one event run in `(extension_name, pos, path)` order:
by extension name across extensions, manifest order within one.

## Order at the end of an iteration

One hold covers all of it:

1. The iteration ended the turn with an answer: `Stop` (or `SubagentStop`)
   hooks, one at a time.
2. The session is over `trim_threshold`: `ContextThreshold` hooks, one at a
   time. The session is measured again before each one; a hook that brought
   it under ends the step.
3. The C check: still over `trim_threshold`, C trims it to `prune_target`
   ([turn-edit.md](turn-edit.md#cs-trim)).
4. The next request is built (mid-turn), or the session goes idle (turn
   end).

`Stop` comes first so that a review that shrinks the turn is seen by the
size check. Between iterations only step 2 applies, and only while the turn
has iterations left (`max_iterations`); an agent with no `ContextThreshold`
hook goes straight to the C check (`advance_session` returns
`ADVANCE_RUN_HOOKS` only when it has one). At turn end it all happens after
the reply is delivered.

```
final assistant message → idle, delivery edges evaluated → reply delivered
  → Stop / SubagentStop hooks → ContextThreshold hooks (while over)
  → C check → idle branch of advance_session (gates, inbox drain, next turn)
```

At turn end the C check has no turn to stop: a session still over its
threshold after the trim is one WARN line (`trim at turn end: …`), and the
next turn's first iteration meets the same check.

`hook_stop_due` decides whether a finished turn gets its stop hooks: the
agent has one for the event, and the session's leaf is an assistant message
that is not an error stop (a provider failure, the iteration or context
cutoff). Nothing about the turn's size or content is judged in C: a script
that finds nothing worth doing returns `{}`, an ordinary success. Every skip
but "no hook" is one INFO line.

## CacheExpiring

A one-shot timer per session, in the database: `sessions.cache_armed_at`.

- **Armed** by every request that was answered (`LLM_RESP_OK`), to the time
  that request started — when the governing entry has a `cache_lifetime`;
  otherwise the arm is cleared (`cache_expiring_arm`, `src/llm_proc.c`).
- **Due** from 90% of the lifetime after that start until 100%. The daemon
  checks it on every pass of its event loop (`cache_expiring_tick`) and
  sleeps no later than the next due time.
- **Fires** only on a session that is `idle` with nothing unconsumed in its
  inbox, and only when the agent has a `CacheExpiring` hook and the session
  is above `prune_target`. The claim is the disarm itself, so it fires once
  per idle period, whatever happens next. A session already running a hook
  child or a tool child is passed over for that period.
- **Lapses**: an arm past 100% of the lifetime, or one whose entry has lost
  its lifetime, is cleared without firing. A session that is busy at 90%
  keeps its arm until its next answered request re-arms it, or until it
  lapses.

Nothing follows it: no C check, no trim. The lifetime is a documented
figure, and a wrong one must cost a wasted summary, never discarded context.
The CLI never fires it.

## The script

Run by the vehicle cron scripts use (`script_child_spawn`): the agent's caps
are re-read, the path must be inside the extension store, only the secrets
the agent is granted ride along, and it runs under the agent's
containment.

- **`args`** — the session as plain data: the JSON `turn_edit_input()`
  builds ([Input and reply](#input-and-reply)). It does not travel in the
  request blob: the parent writes it to an unnamed file and passes the
  descriptor on the request socket (`TOOL_ARG_JSON_FD`, `run_tool.h`). It is
  never longer than the hook's `max_bytes` (default 1 MiB, at most 16 MiB =
  `RUNTOOL_ARG_FD_MAX`): history that does not fit loses its oldest turns,
  and a newest turn that alone does not fit is not handed over at all — the
  hook is "not started" and passed over. A script that asks for a lot of
  history should raise `js_heap_mb` with its `max_bytes`.
- **`getConfig(key)`** — the extension's own settings.
- **`LLM(prompt, opts)`** — bridged to the parent as for any JS child;
  `llm_responses` rows carry the source `hook:<extension>`.
- **Return** the reply as JSON text — `return JSON.stringify({...})`. The
  file is a function body, and the result is the returned string: an object
  returned directly arrives as `[object Object]`, and anything printed with
  `console.log` is prepended to the result. Both make the reply unparsable,
  which is a refusal.

The reply is untrusted input. It reaches the database only through
`turn_edit_apply`, which validates all of it and applies all or nothing.

### Input and reply

The newest turn at the top level, with as much of the conversation before it
as the hook asked for ([turn-edit.md](turn-edit.md#input)). On a stop event
the newest turn has its answer (`final`); on a context event it may still be
running (`final` null).

```json
{"event":"ContextThreshold","session_id":12,"agent":"alavanca",
 "input":{"history":"all","history_results":true,"max_bytes":2097152},
 "context":{"tokens":101200,"window":200000,"trim_threshold":100000,
            "prune_target":50000},
 "turn_id":37,
 "tokens":{"turn":9120,"tools":7400},
 "user_messages":[{"entry_id":900,"text":"…","tokens":40}],
 "notes":[],
 "iterations":[{"iteration_id":411,"entry_id":901,"message_id":902,"text":"…",
   "tokens":30,"reasoning":"…",
   "calls":[{"call_id":"c1","entry_id":903,"tool":"shell_exec","tokens":20,
             "args":{},"protected":false,"is_error":false,
             "result":{"entry_id":905,"text":"…","tokens":2900}}]}],
 "final":null,
 "history":[…],
 "history_omitted":0}
```

`context` is present on the two context events only:

| Field | `ContextThreshold` | `CacheExpiring` | Meaning |
|---|---|---|---|
| `tokens` | yes | yes | the session's size as the C check measures it: head (the provider's measure when a usable report gives one, else the estimate — [memory.md](memory.md#context-window-management)) + every entry the next request would carry |
| `window` | yes | yes | the governing model's context window |
| `trim_threshold` | yes | yes | resolved (defaults filled in) |
| `prune_target` | yes | yes | resolved |
| `depth` | — | yes | `sessions.depth`: 0 = top level |
| `cache_lifetime` | — | yes | seconds, from the governing entry |
| `idle_seconds` | — | yes | seconds since the session's last successful response was stored |
| `cache_read_tokens` | — | yes | that response's `llm_responses.cached_tokens`, null when not reported |
| `cache_write_tokens` | — | yes | its `cache_write_tokens`, null when not reported |

The reply is a list of edits and an optional opaque record
([turn-edit.md](turn-edit.md#reply)):

```json
{"edits":[{"op":"summarise","from_turn":3,"to_turn":20,"text":"…"},
          {"op":"drop","call_id":"c4"}],
 "record":{"…":"…"}}
```

`{}` changes nothing and is a success.

## The hold

For an awaited hook the session is held in state `settling`. The script is
forked first and the state set second, so a hook that cannot start never
moves the session. `settling` is entered from `idle` (turn end,
`CacheExpiring`) or `tool_running` (between iterations), and left only to
`idle` or `tool_running` (`session_set_state`): nothing that starts or
resumes a request can take the session while it is held. The same write
(`session_set_settling`) records the event the hold began with in
`sessions.settle_event` (`Stop`, `SubagentStop`, `ContextThreshold`,
`CacheExpiring`); every other transition clears it. Whoever ends the hold
reads what follows from the row: `turn_iteration` > 0 is between
iterations, and `settle_event` = `CacheExpiring` means no C check.

While held:

- `advance_session` returns `ADVANCE_WAITING` for the state, whatever woke
  it. Messages, cron prompts, script results and sub-agent results queue in
  the inbox and are not consumed; each waits at most the running hook's
  deadline.
- the session counts as busy wherever "not idle" is the test: a co-pointed
  `cclaw -s <id>` is refused, `rename-agent` waits, a cron job's
  one-live-fire interlock holds, the dashboard shows "tidying up".
- the session **holds a resource** for `session_max_concurrent`
  (`session_count_resource_holders` counts `settling`): a script is
  straight-line code, so one held slot is at most one `LLM()` call in
  flight. An async hook holds a child slot (`CHILD_MAX`), not a gate slot.

When the script's child is reaped (`turn_settle_child_done`):

1. An awaited hook's clean exit hands its output to `turn_edit_apply` — if
   this process still holds the session; otherwise the reply is discarded.
2. One log line: `hook <event> ext=<name> outcome=<applied|nothing|rejected|error|failed|discarded|done> reason=…`.
3. The outcome is counted ([Failures](#failures)).
4. The next awaited hook of the step starts, then the next step's
   ([order](#order-at-the-end-of-an-iteration)). Each sees the session as the
   one before left it.
5. With none left the hold ends: mid-turn back to `tool_running`, then the C
   check and the next request (`run_iteration_end`); at turn end to `idle`,
   the C check, then the idle branch of `advance_session` (gates, inbox
   drain, next turn); after `CacheExpiring` to `idle` with no check.

`cclaw -p` does not exit while its session is held. The interactive CLI
shows its prompt as soon as the reply is printed; a line typed during the
hold is queued and its turn starts when the hold ends.

A new message does not cancel a running hook, and nothing else does short of
its deadline.

### Deadline

`hook_timeout` (seconds, default 60), or the hook's own `timeout`, bounds a
script, and so bounds how long a message can wait behind a hold. Two clocks,
as for every sandboxed child: the broker's own timer ends the script at the
deadline and tears the sandbox down; the loop's deadline sweep kills the
child outright five seconds later if it is still there. Either way: nothing
applied, a failure counted.

A script killed while an `LLM()` call is in flight does not stall the daemon:
`llm_bridge_stop` detaches a bridge thread that is mid-request instead of
joining it, and the thread frees the bridge when the provider answers.

### The hold always ends

| What happens | How the session gets back |
|---|---|
| script returns | reap → apply → next hook or release |
| script throws, exits non-zero, is killed by a signal | reap → failure → next hook or release |
| script overruns | broker timer (reap) or the deadline sweep → failure → next hook or release |
| no child slot, fork fails, script file missing, no workspace, the newest turn larger than the hook's `max_bytes` | nothing started, the hook is passed over and the next one tried; with none, the session never left its state. One log line, not counted |
| the session could not be marked `settling` (another process moved it first) | the child runs on unheld; its reply is discarded |
| the process holding it is killed | `db_recover_stale_sessions` — `settling` under a dead owner → `idle`, `settle_event` cleared ([below](#a-hold-cut-short)) |
| the reap never reaches `settle.c` | `turn_settle_sweep`, every periodic tick: a session this process holds in `settling` with no hook child is released exactly as the reap would release it — between iterations to the C check and the next request, at turn end to the C check and idle, after `CacheExpiring` to idle with no check |

A child orphaned by a killed daemon runs until its own timer and its output
goes nowhere.

### A hold cut short

Recovery puts the session back to `idle` and nothing else: no hook runs
again and no C check runs. What happens next depends on where the hold was.

- **Between iterations.** Every call of the iteration was answered before the
  hold began, so the turn is waiting on its next request. The daemon's inbox
  sweep finds it — a session whose leaf, read through notes and summaries
  (`CCLAW_SQL_LEAF_READ_THROUGH`, `advance.h`), is a tool result or a user
  message — and `advance_session` makes that request as the same turn, its
  iteration count re-derived from the entries. A note or summary a hook left
  at the end of the running turn before the crash is part of what is sent.
  The next iteration's end runs the hooks and the C check as usual. Under the
  CLI, which adopts no orphaned work, nothing resumes on its own: the next
  message (`-p … -s <id>`, or a line typed) opens the next turn after the
  interrupted one, which the model then sees as it stands.
- **At turn end, or on `CacheExpiring`.** The answer is already written and
  delivered; the session is idle at rest. Whatever the hook would have done
  is not done.

`test/test_integration_hold_crash.c` kills a daemon and a CLI run with
`SIGKILL` while each holds a session this way, with a hook's note as the
leaf, and checks both outcomes.

### Not waited for

`"async": true` — the script is started with the same input, sandbox and
deadline, the session is not held, and whatever it returns is discarded. The
async hooks of a step all start together, before its first awaited one. A
crash or a timeout still counts as a failure; a clean exit is a success.

## Failures

A run fails when the script times out, crashes, throws, exits non-zero, or —
for an awaited hook — its reply is `rejected` (unparsable, or a check in
[turn-edit.md](turn-edit.md#reply) failed). `applied` and `nothing` are
successes. Counted as neither: a hook that could not be started, a reply
discarded because the hold was lost, and `error` (a failure on our side,
such as a database error during apply).

`hooks.fail_count` counts consecutive failures of one hook across every
session — a broken script fails for all of them — and a success zeroes it.
The third in a row (`HOOK_FAIL_LIMIT`) sets `hooks.disabled = 1`: the hook
no longer runs for any agent. Exactly one run trips it (the flag is set by a
statement keyed on it still being clear), and that run puts one note in the
inbox of the default agent's most recent session (`notify_disabled`) and
asks the agent to tell the operator. The note names the one hook — event,
script and extension; the extension's other hooks still run — the last
failure, what goes without it, and the ways to turn it back on below, the
statement written for exactly that hook's row. The agent whose session it
was is never told: it cannot fix an extension's script, and its session is
intact.

While a `ContextThreshold` hook is disabled its agents are on C's trim alone;
while a `CacheExpiring` hook is, idle sessions are left as they are.

**Re-enabling.** Use the explorer's **Turn back on**, beside the hook on
the extension's page or the agent's (action `hook_enable`,
[explorer.md](explorer.md#hooks)); clear the flag on that one row:

```sql
-- cclaw sqlite3 --write ~/.cclaw/cclaw.db "…"
UPDATE hooks SET disabled=0, fail_count=0
 WHERE extension_name='<ext>' AND event='<event>' AND path='<path>';
```

or, for an extension an agent owns, have it promoted again (its rows are
rewritten). An extension no agent owns — the bundled `compaction`, one the
operator installed — has nobody to promote it; the button or the statement
is the way back.

A bundle shipped in the binary keeps its flag and count across restarts of
the same version; a build with a new version of the bundle clears both.
`hooks.enabled` is a separate switch, the operator's, never set by the
runtime.

## Shipped examples

- `extensions/compaction` — `ContextThreshold` and `CacheExpiring`, embedded
  in the binary and attached to every agent: summarises the oldest part of
  the session down to the prune target with the agent's own model. See its
  README and [memory.md](memory.md#compaction).
- `extensions/turn-review` — a `Stop` hook, attached by hand: asks a utility
  model which tool calls of the finished turn to drop and which result lines
  to keep, builds the excerpts in JavaScript, and returns `{}` whenever the
  model's answer is in doubt. `test/test_turn_review_ext.c` runs the shipped
  file against a mock provider.

## Configuration

| Key | Default | |
|---|---|---|
| `hook_timeout` | 60 | seconds before a script is ended, unless its manifest entry says `timeout`; raise on a slow box or link |

Read at every hook start. It measures the machine and the provider (a script
that calls `LLM()` waits on the network), so it is a setting, not a
constant.

## Security

A hook script has exactly what a tool call by the same agent has: the
agent's containment, its grants, its granted secrets, egress through the
proxy. Promoting the extension is the approval. What it is shown is the
session's own entries — all of them already scanned on their way in — as
far back as its manifest asks; the approval line says how much. What it can
change is what `turn_edit_apply` allows: the session's structure stays
legal, unfinished and protected calls stay, the newest turn's answer is never
edited, originals are never modified or deleted. All text it supplies is
scanned for secrets and refused on a hit, never stored masked; its notes are
labelled as the extension's on the wire, so they never carry the runtime's
`[system]` authority.
