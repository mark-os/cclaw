# Hooks

A hook is a script an extension ships that the runtime runs when an event
happens. It is forked, in the agent's sandbox, exactly like a scheduled
script: no extension JavaScript runs inside the daemon. One event exists —
`turnSettled`.

Code: `src/hook.c` (which hooks, when, the failure count), `src/settle.c`
(the event and the hold), `src/dispatch.c` (`hook_script_spawn`, the fork and
the bytes). What a `turnSettled` script is given and what it may return is
summarised [below](#input-and-reply) and defined in full in
[turn-edit.md](turn-edit.md); the mechanism here depends on it only through
two calls in `src/settle.c` — `turn_edit_input` to build `args`,
`turn_edit_apply` to apply the reply.

## Registering one

A `hooks[]` entry in the extension manifest ([extensions.md](extensions.md)):

```json
"hooks": [ { "event": "turnSettled", "handler": "review.qjs", "wait": true } ]
```

- `event` — `turnSettled`. `extension_manifest_validate` refuses any other
  name: a hook registered under an event that never fires would be a silent
  lie.
- `handler` — a `.qjs` file in the bundle.
- `wait` — boolean, default `true`. Whether the session is held until the
  script ends. See [Not waited for](#not-waited-for).

Promote writes one `hooks` row per entry ([schema.md](schema.md#hooks)):
`(extension_name, event, path, enabled, wait, fail_count)`, `path` inside the
shared store. Re-promoting replaces the rows, so the failure count starts
over with the new code.

**Which hooks an agent runs** is read from the tables when the event happens
(`hook_next`), never cached: `hooks ⨝ agent_extensions ⨝ extensions` where
the extension is attached to the agent and enabled there, the hook and the
extension are enabled, and the extension is published or owned by that agent
— the rule its tools load by. An extension attached a moment ago is found on
the next turn without a restart, and two agents sharing a daemon each run
their own.

## turnSettled

Fires at the end of a turn that completed normally, after the reply has been
delivered and before the compaction check (`run_advance`, `ADVANCE_DONE`):

```
final assistant message → session idle, delivery edges evaluated
  → reply delivered → turnSettled hooks → compaction check
```

`hook_turn_settled_due` decides whether this turn gets them. All of:

| Condition | Otherwise |
|---|---|
| the session's agent has an enabled `turnSettled` hook | nothing happens; one indexed lookup per turn |
| the session is top-level (no parent) | a sub-agent's turn never gets hooks |
| the turn ended with an assistant message that is not an error stop | a provider failure, the iteration cutoff and the context cutoff are not "settled" |

Nothing about the turn's size or content is judged in C. Whether a turn is
worth a script's attention is the script's own call: it returns `{}` for one
that is not, which costs a fork (~10 ms) and is an ordinary success. When a
condition fails the session never leaves `idle` for this and the compaction
check runs exactly as it would without hooks. Every skip but "no hook" is one
INFO log line with the reason.

### The script

Run by the vehicle cron scripts use (`script_child_spawn`): the agent's caps
are re-read, the path must be inside the extension store, only the secrets
the agent is granted ride along, and it runs under the agent's sandbox
profile.

- **`args`** — the finished turn, the JSON `turn_edit_input()` builds
  ([Input and reply](#input-and-reply)). It can be far
  larger than a tool call's arguments, so it does not travel in the request
  blob: the parent writes it to an unnamed file and passes the descriptor to
  the child on the request socket (`TOOL_ARG_JSON_FD`, `run_tool.h`). The
  script sees an ordinary `args` object. A turn whose JSON exceeds 16 MB
  (`RUNTOOL_ARG_FD_MAX`) is not handed over at all — the hook is "not
  started" and the turn left alone. In practice the script's heap
  (`js_heap_mb`, default 8 MB) is the tighter bound on a very large turn; a
  script that runs out of it fails like any other.
- **`getConfig(key)`** — the extension's own settings.
- **`LLM(prompt, opts)`** — bridged to the parent as for any JS child;
  `llm_responses` rows carry the source `hook:<extension>`.
- **Return** the reply as JSON text — `return JSON.stringify({...})`. The
  file is a function body (`return` at top level), and the result is the
  returned string: an object returned directly arrives as `[object Object]`,
  and anything printed with `console.log` is prepended to the result. Both
  make the reply unparsable, which is a refusal.

The reply is untrusted input. It reaches the database only through
`turn_edit_apply`, which validates all of it and applies all or nothing;
the bytes the script wrote are handed to it unmodified.

### Input and reply

`args` is the turn as plain data ([turn-edit.md](turn-edit.md#input)):

```json
{"event":"turnSettled","session_id":12,"agent":"alavanca","turn_id":37,
 "tokens":{"turn":9120,"tools":7400},
 "user_messages":[{"entry_id":900,"text":"…"}],
 "notes":[{"entry_id":930,"text":"…","before_iteration":412}],
 "iterations":[{"iteration_id":411,"entry_id":901,"text":"…",
   "calls":[{"call_id":"c1","entry_id":903,"tool":"shell_exec","args":{},
             "protected":false,"is_error":false,
             "result":{"entry_id":905,"text":"…","tokens":2900}}]}],
 "final":{"entry_id":960,"iteration_id":413,"text":"…"}}
```

Result text is verbatim. `protected` calls cannot be dropped (effect tools,
`check_session`, a `save_secret` call, an unfinished call).

The reply is a list of edits and an optional opaque record
([turn-edit.md](turn-edit.md#reply)):

```json
{"edits":[{"op":"drop","call_id":"c4"},
          {"op":"replace","call_id":"c1","text":"…"},
          {"op":"note","text":"…","before":901}],
 "record":{"…":"…"}}
```

`{}` changes nothing. A note is stored as a `system` entry with
`data.source = "hook:<extension>"` and reaches the model as
`[note from extension <extension>] …`, not `[system] …`. The runtime's record
of what changed, with the script's `record` inside, is appended to
`$.turn_edits` on the turn's final assistant entry.

### The hold

For a hook with `wait: true` the session is held in state `settling`
(`turn_settle_begin`). The script is forked first and the state set second,
so a hook that cannot start never moves the session.

While held:

- `advance_session` returns `ADVANCE_WAITING` for the state, whatever woke
  it. Messages, cron prompts, script results and sub-agent results queue in
  the inbox and are not consumed.
- `session_set_state` lets `settling` be entered from `idle` only and left to
  `idle` only, so nothing that starts or resumes a turn can take the session.
- the session counts as busy wherever "not idle" is the test: a co-pointed
  `cclaw -s <id>` is refused, `rename-agent` waits, a cron job's
  one-live-fire interlock holds, the dashboard shows it working ("tidying
  up"). It is not a resource holder for `session_max_concurrent` — like a
  scheduled script, it holds a child slot (`CHILD_MAX`), not a gate slot.

When the script's child is reaped (`turn_settle_child_done`):

1. A clean exit hands its output to `turn_edit_apply`.
2. One log line says what happened and why:
   `hook turnSettled ext=<name> outcome=<applied|nothing|rejected|error|failed> reason=…`.
3. The outcome is counted ([Failures](#failures)).
4. The agent's next awaited hook is started, if it has one. Several run one
   at a time in `(extension, path)` order, each given the turn as the one
   before left it, and each may edit it again (one more `$.turn_edits`
   element). A hook whose reply is `rejected` is logged and counted, and the
   chain goes on.
5. With none left the hold ends the way a finished compaction ends: the
   session goes `idle`, the deferred compaction check runs against the turn
   as it now is, and otherwise the idle branch of `advance_session` runs its
   gates, drains the inbox and opens the next turn.

`cclaw -p` does not exit while its session is held: a scripted run sees the
same turn the daemon would leave. The interactive CLI shows its prompt as
soon as the reply is printed; a line typed during the hold is queued and its
turn starts when the hold ends.

### Deadline

`turn_settled_timeout` (seconds, default 60) bounds a script, and so bounds
how long a message can wait behind a hold. Two clocks, as for every sandboxed
child: the broker's own timer ends the script at the deadline and tears the
sandbox down cleanly; the loop's deadline sweep kills the child outright five
seconds later if it is still there (a broker that is itself stuck). Either
way: nothing applied, turn untouched, a failure counted.

A script killed while an `LLM()` call is in flight does not stall the daemon:
`llm_bridge_stop` detaches a bridge thread that is mid-request instead of
joining it, and the thread frees the bridge when the provider answers.

### The hold always ends

| What happens | How the session gets back to `idle` |
|---|---|
| script returns | reap → apply → release |
| script throws, exits non-zero, is killed by a signal | reap → failure → release |
| script overruns | broker timer (reap) or the deadline sweep → failure → release |
| no child slot, fork fails, script file missing, no workspace | nothing started; the session never left `idle`. One log line, not counted as a failure |
| the session could not be marked `settling` (another process opened a turn first) | the child is demoted to not-waited-for; its reply is discarded |
| daemon killed mid-hold | `db_recover_stale_sessions` — `settling` under a dead owner → `idle`, turn and inbox untouched, no resume nudge (no turn was in flight) |
| the reap never reaches `settle.c` | `turn_settle_sweep`, every periodic tick, in the daemon and the CLI: a session this process holds in `settling` with no hook child is released |

A child orphaned by a killed daemon runs until its own timer and its output
goes nowhere.

### Not waited for

`wait: false` — the script is started with the same input, the same sandbox
and the same deadline, the session is not held, and whatever it returns is
discarded: it cannot change the turn. Unawaited hooks are all started
together, before the first awaited one. A crash or a timeout still counts as
a failure.

### Failures

A run fails when the script times out, crashes, throws, or — for an awaited
hook — its reply is `rejected` (unparsable, or a check in
[turn-edit.md](turn-edit.md#reply) failed) or apply returned `error`.
`applied` and `nothing` are successes. A hook that could not be started is
neither.

`hooks.fail_count` counts consecutive failures of one hook across every
session — a broken script fails for all of them — and a success zeroes it.
The third in a row (`HOOK_FAIL_NOTIFY`) sends one notice to the operator of
the session it happened on (`channel_notify_operator`; a no-op for a session
with no channel), once per streak, the way failing compaction reports. The
agent is never told: it cannot fix an extension's script, and its turn is
intact.

## A shipped example

`extensions/turn-review` is a complete `turnSettled` hook: it decides for
itself whether a turn is worth a model call, asks the agent's utility model
which tool calls to drop and which result lines to keep, builds the excerpts
in JavaScript, and returns `{}` whenever the model's answer is in doubt. Its
README covers attaching it and reading what it did;
`test/test_turn_review_ext.c` runs the shipped file against a mock provider.

## Configuration

| Key | Default | |
|---|---|---|
| `turn_settled_timeout` | 60 | seconds before a script is ended; raise on a slow box or link |

Read at every hook start, so a change takes effect on the next turn. It
measures the machine and the provider (a script that calls `LLM()` waits on
the network), so it is a setting, not a constant.

## Security

A hook script has exactly what a tool call by the same agent has: the
agent's sandbox profile, its grants, its granted secrets, egress through the
proxy. Promoting the extension is the approval. What it is shown is the
session's own latest turn, already scanned on its way in. What it can change
is what `turn_edit_apply` allows: drop unprotected calls of that turn, replace
result text, add notes. All text it supplies is scanned for secrets and
refused on a hit, never stored masked; its notes are labelled as the
extension's on the wire, so they never carry the runtime's `[system]`
authority.
