# Turn edit — what a hook sees and may change

`src/turn_edit.c`. `turn_edit_input` builds the session as plain data for a
[hook](hooks.md) script: the newest turn, and as much of the conversation
before it as the hook asked for. `turn_edit_apply` checks the script's reply
and edits the session. `turn_edit_trim` is C's own fallback, which goes
through the same checks and the same writing. None of them runs a script or
changes session state; `src/settle.c`, `src/advance.c` and `src/loop.c` call
them, and `session_condense` is a thin caller of the summarise path.

A session carries material later requests do not need: a search that found
nothing, 300 lines of log of which five mattered, forty turns of a finished
task. A script can take that off the session's active branch, replace a
result with shorter text, rewrite a message, leave a note, or replace a run
of turns or steps with a summary it wrote. Rows are re-pointed or added,
never modified or deleted — the originals stay in the table.

What an edit is *for* is the script's business: prompts, thresholds,
excerpts and wording live in the extension's JavaScript. C checks structure
and keeps the record, never content.

## Input

`turn_edit_input(db, session_id, &spec, &what, &too_big)` returns the
script's `args`. `spec` is the hook's declared input (`TurnEditInputSpec`,
from `hooks.input` by `hook_next`); `what` says the event, whether the
newest turn must have ended with an answer (the stop events), and the
`context` object of the context events ([hooks.md](hooks.md#input-and-reply)).

### What a hook asks for

```json
{"event": "Stop", "handler": "review.qjs",
 "input": {"history": 5, "history_results": false, "max_bytes": 2000000}}
```

| Option | Values | Default | |
|---|---|---|---|
| `history` | `"none"`, a positive integer N, `"all"` | `"none"` | earlier turns sent with the newest one: none, the last N, or the whole active branch, summaries included |
| `history_results` | boolean | `true` | `false`: earlier turns' tool results come without their text |
| `max_bytes` | integer, 1024 … 16777216 | 1048576 (1 MiB) | the most the whole input may be, in bytes of JSON text |

Every option is optional, and so is `input` itself. Promote refuses anything
else, naming it. The declaration is stored as given in `hooks.input` (NULL
when absent); the defaults are filled in each time the hook runs.

**The defaults, and why.** 1 MiB is about the most a script on the default
QuickJS heap (`js_heap_mb` 8) can parse — the parsed value costs several
times its text. 1024 is the least: below it the JSON around the smallest
turn does not fit. 16 MiB (`TURN_EDIT_INPUT_MAX_BYTES`) is the largest
input the hook vehicle carries (`RUNTOOL_ARG_FD_MAX`; `settle.c` asserts
they are equal).

### The rules

1. **The newest turn is always sent whole**, every result with its text. If
   it alone does not fit in `max_bytes`, nothing is built: NULL with
   `too_big` set, and the hook is "not started" — one log line, passed over,
   not counted ([hooks.md](hooks.md#the-hold-always-ends)). On a stop event
   nothing is built either when the newest turn did not end with an
   assistant message.
2. **`history`** selects the earlier part of the branch. `"none"` sends `[]`
   and reads no entry outside the newest turn. N is the last N turns, oldest
   first, with any summary standing among them. `"all"` is the whole branch.
3. **`history_results: false`** keeps every earlier call, its `args` and
   its `result` object, but the `result` has no `text` —
   `{"entry_id":905,"tokens":2900}`. The newest turn's results are never cut.
4. **`max_bytes`** caps the whole input. History that does not fit loses
   its oldest turns first, whole. A summary between turns is kept in
   preference to every turn older than it.
5. **The input says what it was given**: `input` echoes the resolved options,
   and `history_omitted` counts the requested earlier turns left out for size
   (summaries are not counted).

**What `max_bytes` bounds, precisely**: the length of the JSON text handed
to the script — the returned string is never longer. One statement decides
it: a pass over integers keeps the earlier units, newest first, whose stored
text (`content_bytes`) could still fit beside the newest turn; text is then
read and serialised for those only, and they are kept, newest first, while
the bytes actually written out fit. What is measured is exactly what is
handed over.

### Shape

```json
{"event":"Stop","session_id":12,"agent":"alavanca",
 "input":{"history":"all","history_results":true,"max_bytes":1048576},
 "turn_id":37,
 "tokens":{"turn":9120,"tools":7400},
 "user_messages":[{"entry_id":900,"text":"…","tokens":40}],
 "notes":[{"entry_id":930,"text":"…","tokens":12,"before_iteration":412}],
 "iterations":[
   {"iteration_id":411,"entry_id":901,"message_id":902,
    "text":"Let me look at the logs.","tokens":30,"reasoning":"…",
    "calls":[{"call_id":"c1","entry_id":903,"tool":"shell_exec","tokens":25,
              "args":{"command":"…"},"protected":false,"is_error":false,
              "result":{"entry_id":905,"text":"…","tokens":2900}}]}],
 "final":{"entry_id":960,"iteration_id":413,"text":"…","tokens":200,
          "reasoning":"…"},
 "history":[
   {"summary":{"entry_id":880,"text":"…","tokens":1200}},
   {"turn_id":36,"tokens":{…},"user_messages":[…],"notes":[…],
    "iterations":[…],"final":{…}}],
 "history_omitted":0}
```

A context event adds `context` after `input`
([hooks.md](hooks.md#input-and-reply)).

- `tokens` — on every entry, its stored count (`entries.tokens`) or, where
  unmeasured, its estimate; per turn, the whole turn's and its `tool_call` +
  `tool_result` entries'. An iteration's `tokens` is its assistant text's.
- `user_messages` — the turn's `user_message` entries, in order.
- `notes` — `system` entries in the turn (runtime notices, notes a hook or
  the trim left) and summaries of some of its iterations (marked
  `"summary": true`), each with the id of the iteration it stands before
  (`before_iteration`; null when nothing follows it).
- `iterations` — every iteration but the answer's, in order. `entry_id` is
  its first row on the branch (its `reasoning` row when it has one) — what a
  note's `before` and a summary's `from_iteration`/`to_iteration` name by
  iteration id. `message_id` is its assistant message. `text` is the
  assistant text (`""` when none). `reasoning` is the readable summary of
  the model's reasoning for that response, present only when one was kept
  (`agent_models.reasoning_display`); never the thinking itself, its
  signature or the replay blob. `calls` are in call order; `args` is the
  parsed object, or the stored string when it is not JSON. A result is paired
  with its call by call id within the turn: a sub-agent's answer or an
  approval notice is appended under an iteration id of its own.
- `final` — the turn's answer: its newest row that is not a note, when that
  is an assistant message. Null while the turn is still running (a context
  event between iterations) and for a turn that ended on a cutoff.
- `history` — earlier turns in the same shape, and `{"summary":{…}}` where a
  summary stands between turns, oldest first. An assistant entry that
  stopped on an error is left out of an earlier turn (with its reasoning),
  as every request leaves it out.
- `protected` — see [below](#protected-calls).

`history` is the active branch as the agent now has it, edits included,
not the session as first written. It can be longer than what the next
request carries — the request's cut and the head (system prompt, tools,
user context block) are not in it.

### Protected calls

`protected: true` means the call cannot be dropped on its own. It may still
be rewritten and its result replaced, and it leaves with a whole turn or
iteration.

- The tool is not droppable: `tools.droppable` is 1 only for tools whose
  purpose is to look something up. A call made for its effect — a file
  written, a message sent, an agent launched, a memory note, a config
  request, a schedule — is the record that the effect happened. Built-in
  tools declare it in their C recipe, an extension tool with
  `"droppable": true` in its manifest; a tool with no `tools` row is
  protected. `check_session` is protected.
- The call passed `save_secret` — read on the call as the model made it, so
  a rewrite of its arguments cannot make it droppable.
- The call's `tool_calls` row is not `done` (`pending`, `running`, or a
  detached `background` job).

## Reply

`turn_edit_apply(db, session_id, reply_json, source_label, &err)`.
`source_label` is `hook:<extension>`, or `tool:session_condense`.

```json
{"edits":[{"op":"drop","call_id":"c4"}],
 "record":{"…":"…"}}
```

`record` is an optional object (at most 16 KB, `TURN_EDIT_RECORD_MAX_BYTES`)
stored untouched with the runtime's record of the edit. It is never sent to
the model. Returns `TURN_EDIT_APPLIED`, `NOTHING`, `REJECTED` or `ERROR`;
only `APPLIED` writes anything.

**Nothing to do** — `{}`, `{"edits":[]}`, or a record with no edits. Shape
checks run first, so a malformed record with no edits is `REJECTED`.

### Operations

Valid on any turn of the active branch, between iterations of a running turn
as at a turn's end. A `call_id` is looked up in the newest turn unless
`turn_id` names another: providers reuse call ids across turns.

| op | fields | does |
|----|--------|------|
| `drop` | `call_id`, `turn_id`? | the call and its result leave the branch |
| `replace` | `call_id`, `turn_id`?, `text` | a new result with this text in the old one's place |
| `rewrite` | `entry_id`, `text` | new text for a user message, an assistant message, a note or a summary |
| `rewrite` | `call_id`, `turn_id`?, `args` | new arguments (an object) for a call |
| `note` | `text`, `before`? | a system note before an iteration |
| `drop_turns` | `from_turn`, `to_turn` | a run of whole turns before the newest leaves |
| `summarise` | `text`, `from_turn`, `to_turn` | a run of whole turns before the newest is replaced by a summary |
| `summarise` | `text`, `from_iteration`, `to_iteration` | a run of whole iterations of the newest turn is replaced by a summary |

```json
{"op":"drop","call_id":"c4"}
{"op":"replace","call_id":"c1","turn_id":30,"text":"the 5 lines that mattered: …"}
{"op":"rewrite","entry_id":900,"text":"Deploy the fix to staging."}
{"op":"rewrite","call_id":"c2","args":{"path":"notes.md"}}
{"op":"note","text":"Dropped two empty searches.","before":901}
{"op":"drop_turns","from_turn":3,"to_turn":9}
{"op":"summarise","from_turn":3,"to_turn":20,"text":"The user asked … We decided …"}
{"op":"summarise","from_iteration":411,"to_iteration":430,"text":"Read the logs; …"}
```

- **`drop`** — the call must be finished (`done`), droppable and carry an
  iteration id. Every result with its call id in that turn leaves with it.
  An iteration left with no call leaves whole — its assistant text,
  reasoning and results included (the newest turn's answer excepted).
- **`replace`** — the call must not be `pending` or `running`, and must have
  exactly one result.
- **`rewrite`** — by `entry_id`: a `user_message`, `assistant_message`,
  `system` note or `compaction` summary on the branch, never the newest
  turn's answer (it has been delivered, and delivery keys on it); a result
  is changed with `replace`, a call by `call_id` with `args`. A rewritten
  user message keeps its turn. By `call_id`: the call must not be `pending`
  or `running`.
- **`note`** — `before` is the `entry_id` of an iteration's first row, in
  any turn (an `iterations` item's `entry_id`), or the newest turn's answer
  (`final.entry_id`). Without it the note goes before the newest turn's
  answer, or, while the turn runs, at the end of the branch, where the next
  response reads it. Before the answer means before its reasoning too, named
  or not: the reasoning stays next to the answer. Refused before an
  iteration this reply empties. Several notes at one place land in edit
  order.
- **`drop_turns`, `summarise` by turn** — turn ids, oldest to newest, a
  contiguous run of the branch, all before the newest turn. The summary is
  one entry where the run was, carrying the first replaced turn's id: it
  opens no turn, and later trims treat it as the oldest content.
- **`summarise` by iteration** — iteration ids of the newest turn, in branch
  order; never the iteration that holds the turn's answer (while the turn
  runs, its newest iteration may go too). The notes and
  summaries standing directly before `from_iteration` (in `notes` with
  `before_iteration` = `from_iteration`) leave with the run, so a turn
  summarised again and again carries one summary: fold their text into
  yours. The summary carries the newest turn's id. Rows before the turn's
  first iteration (its user messages) stay.

### What C checks

Only these, for every operation, a script's or the trim's:

1. **Pairing.** A call and its result leave together. An iteration left with
   no call leaves whole. An iteration that keeps some of its calls, or has a
   call or its text rewritten, loses its `reasoning` entry: a provider's
   reasoning blob is bound to the full set of calls it was produced with.
2. **Unfinished calls.** A `pending` or `running` call blocks whatever
   would remove it, its whole turn or iteration included. A `background`
   job does not: it has its "started" result and is answered later in the
   inbox.
3. **Effect calls.** A protected call cannot be dropped on its own; it may
   be rewritten and may leave with its whole turn or iteration.
4. **The record.** Originals are never modified or deleted, and every edit
   appends a record ([below](#the-record)).

And two rules of the reply itself: the newest turn's answer is never edited
or removed, and each entry is named by one edit at most (including the rows
an operation takes with it).

### Rejected whole

One failed check refuses the entire reply, with a one-line reason:

- shape (`edits[N] …`): not a JSON object; a top-level key other than
  `edits` and `record`, or one given twice; `edits` not an array; `record`
  not an object, or too large; an edit that is not an object, has an unknown
  `op`, names a field twice, has a field its op does not take, lacks one it
  needs, or has one of the wrong type (ids integers, `call_id` a string,
  `text` a non-empty string, `args` an object);
- the operations (`drop: …`, `summarise: …` …): an id not on the branch or
  not of the right kind; a call id naming more than one call; an entry named
  by two edits; any of the checks above;
- text (`edits[N]: …`): invisible characters are stripped from `text`
  first, and text empty after that is refused; then `text`, and a call's
  `args` as JSON, go through `tool_result_postprocess` with a fresh
  `secrets_snapshot` — stored secret values and the scanner's rules, the
  chain `inbox_insert_scanned` runs. A hit refuses the reply; text is never
  stored masked;
- the session has no entry.

`ERROR` is a failure on our side (a DB error); nothing is written. The
reply's shape and text are checked without a transaction. Everything that
reads the branch runs inside one `BEGIN IMMEDIATE`, so what is checked is
what is edited; the function refuses to run inside a caller's transaction.

## What is written

All in that one transaction.

- **A stand-in** (a replaced result, a rewritten message, call, note or
  summary) is a new row in the original's place, carrying its type, role,
  `turn_id`, `iteration_id`, `part_index`, `tool_call_id`, `tool_name`,
  `is_error`, `stop_reason`, `model`, `network_hosts` and `fence_id` — so a
  replaced network result keeps its untrusted-content fence. Not the usage
  and cost columns: they belong to the response the original was.
- **A note** is an ordinary `system` entry: role 0, `iteration_id` 0, the
  turn of the place it goes.
- **A summary** is a `compaction` entry: role 4, no iteration, sent as user
  text under `[Summary of earlier conversation]`.

The branch is rebuilt front to back: each entry that stays hangs off the
entry that now precedes it. Before `COMMIT` it is walked again from the
leaf: it must be exactly the expected chain, hanging off the same root;
otherwise the transaction rolls back and the call returns `ERROR`. The leaf
is the last row of the new chain — at a turn's end the answer, unchanged.

Not changed: `tool_calls` rows, any original row's content, usage or cost.

### Edited entries

Every entry an edit wrote has `data.edited_by` = the edit's label
(`hook:<ext>`, `fallback` for the trim, `tool:session_condense`). A note or
summary also has `data.source` = the same label: the edit is where its
words came from. A stand-in keeps the original's `data` keys — its
`data.source` among them, so a rewritten cron prompt is still the cron's
and the runaway-turn guard, which reads the source of user messages, still
counts its turn as autonomous — and has `data.replaces` = the id of the first
original it stands for, through any chain of rewrites
(`TURN_EDIT_SQL_STANDS_IN` tests it). A reader that counts events by source
skips stand-ins: the cron failure streak counts one fire once. Notes (role 0)
and summaries (role 4) are never user messages to any reader that picks
entries by role. An original re-pointed past removed rows has
`original_parent_id` = the parent it was written under (the first one, if
re-pointed again). An original that left the branch is not touched and still
points at its own parent, so the session as written can always be walked
back.

### The record

One object appended to `$.turn_edits` in the `data` of one entry: the
session's leaf after a script's edit (at a turn's end, its answer), the note
the trim wrote. Other keys there are kept. Never sent to the model.

```json
{"by":"hook:compaction","at":1763300000,
 "left_branch":[412,415,416],
 "replaced":[{"original":910,"replacement":971}],
 "notes":[972],
 "summaries":[973],
 "tokens":{"before":91200,"after":43100},
 "thinking_cut":973,
 "record":{"…":"the script's own, when it sent one"}}
```

- `left_branch` — every original that left the branch, in chain order,
  replaced and rewritten originals included.
- `replaced` — `{original, replacement}` per stand-in; `notes`,
  `summaries` — the entries written. All in edit order.
- `tokens` — the branch's size before and after, each entry at its stored
  count or its estimate (the trim: the session's size as it measures it,
  head included).
- `thinking_cut` — the first entry of the new branch that is new or
  re-pointed; null when only the tail went ([Thinking](#thinking-after-an-edit)).
- The trim adds `dropped` `{turns, iterations}` (this trim alone), `total`
  (the same, with every earlier trim note it folded: what its text says)
  and `limits` `{window, threshold, target}`.

The record's host is always at least as new as every usage report it
changed the ground of, which is how accounting and the head measure tell
that an edit came between two reports.

## C's trim

`turn_edit_trim`, the fallback. It runs as the C check at the end of every
iteration and at turn end, after the hooks ([hooks.md](hooks.md#order-at-the-end-of-an-iteration)),
and acts when the session is over `trim_threshold`: no `ContextThreshold`
hook, a hook that failed or is disabled, or one that left the session over.
No model call; it reads no policy but the two numbers and works whatever
they are (a `prune_target` at or above the threshold cuts to the threshold).

The size is what the next request would carry (`context_measure`): the
head — the provider's measure where a usable report gives one, else the
estimate — plus every entry at its stored count or estimate, 0 for a
response that stopped on an error and its results. Under the threshold it
returns without a transaction.

1. **Turns.** Drop the oldest whole turns before the newest, down to
   `prune_target`. A turn holding a `pending` or `running` call is skipped.
2. **Iterations.** If still over, drop the oldest whole iterations of the
   newest turn, skipping any that is blocked. The entries before its first
   iteration (the user messages that opened it) and its newest iteration
   always stay.
3. **The note.** One `system` note (`data.source = "fallback"`, sent as
   `[system]`) before the first entry kept after the first one dropped —
   inside the newest turn when its iterations went, else where the turns
   were: "*N earlier turns and M earlier steps of this turn were dropped to
   fit the context. What they said is no longer in front of you; check
   before repeating an action.*" A long turn is trimmed again and again:
   when this trim dropped iterations, an earlier trim's note among the
   turn's opening entries leaves the branch and its running total (its
   record's `total`) is carried into the new note's text, so the model reads
   one running total. A trim note a script rewrote is the script's text and
   stays. The record sits on the note; its `dropped` counts this trim alone,
   its `total` what the text says.
4. **Still over the threshold.** Between iterations the turn stops: an
   error-stop assistant entry ("*error: context limit reached (~N tokens
   after trimming, over this agent's T-token trim threshold) — work was cut
   mid-flight …*") ends it and is delivered on every edge. At turn end there
   is no turn to stop: one WARN log line. This is also the only way out when
   the newest iteration alone exceeds the threshold (one large parallel
   batch), since step 2 always keeps it.

When nothing could be dropped at all, nothing is written and the result is
step 4. The note carries no record of effects; that risk belongs to the
fallback only (the bundled compaction lists them).

The request cut (`context_plan`) uses the same line as its budget —
`trim_threshold`, or `max_history_tokens` when that is set — so on a turn's
first request, before any trim has run, the request still fits.

## Accounting of written entries

A stand-in keeps the original's measured count scaled to its length
(`tokens × new_bytes / content_bytes`) when the original was measured and is
at least 256 bytes; otherwise it is unmeasured and reads as its estimate. A
note or summary has only its estimate. None is measured later: the next
report after an edit is not compared with the one before it
([memory.md](memory.md#context-window-management)), and later reports
measure only what was appended after them. What left the branch took its
count with it. The trigger, the C check and the trim all read
`COALESCE(tokens, token_estimate)`.

## Thinking after an edit

An edit that changes the branch at or before a response's reasoning, after
that reasoning was produced, leaves it bound to a conversation that is no
longer sent. The record's `thinking_cut` marks the point; the payload
builder (`SQL_TRANSCRIPT_STALE`, `src/llm_payload.c`) marks stale every
reasoning entry older than the record's host that stands at or after the
cut. What is stale is always a trailing run of the blocks that existed when
the edit was made. Per wire format:

| Format | After an edit |
|---|---|
| Anthropic | stale thinking is not replayed: the iteration is rebuilt from its entries without thinking, and the model re-thinks from the edit point. `drop_block` (sent on every request) is the backstop |
| OpenAI-compatible | unchanged: DeepSeek requires `reasoning_content` back inside a tool loop and does not check it against the history; OpenRouter's `reasoning_details` carry Gemini signatures the call they rode on needs |
| Gemini | unchanged: a kept call keeps its `thoughtSignature`, even when the edit took its iteration's reasoning entry off the branch; Gemini checks the signature's presence, not the history |

Scripts never see signatures or replay blobs.

## On the wire

A hook's note is sent like any mid-conversation system entry — user-role
text — but labelled by its source: `[note from extension <ext>] <text>`.
Every other `system` entry, the trim's note included, keeps `[system] <text>`
(`SQL_SYSTEM_NOTE_PREFIX`, `src/llm_payload.c`); so does one whose `data` is
not valid JSON. A hook's note is typically written by a model that has just
read untrusted tool output; `[system]` would lend that text the runtime's
own authority. A summary is sent as `[Summary of earlier conversation]` and
its text. The prompt cache restarts at the first changed entry.
