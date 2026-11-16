# Turn edit — what a turnSettled script sees and may change

`src/turn_edit.c`. Two functions: `turn_edit_input` builds the finished turn,
and as much of the conversation that led up to it as the hook asked for, as
plain data for a [turnSettled](hooks.md) script, and `turn_edit_apply` checks the script's
reply and edits the turn. They run no script and change
no session state; `src/settle.c` calls them.

A finished turn can carry material the rest of the session does not need: a
search that found nothing, 300 lines of log of which five mattered. A script
can take that off the session's active branch, replace a result with shorter
text, or leave a note. Rows are re-pointed or added, never deleted — the
originals stay in the table, as with compaction.

What an edit is *for* is the script's business: line numbering, excerpts,
prompts, size thresholds and wording live in the extension's JavaScript. The
runtime enforces only what must not be left to an untrusted script: edits
touch only the finished turn; a call leaves with its result; the answer and
the user messages are never edited; protected calls are not dropped; text a
script supplies passes the secret scanner; all or nothing.

`extensions/turn-review` is the reference script built on this contract.

## Input

`turn_edit_input(db, session_id, &spec, &too_big)` returns the script's
`args`: the newest turn of the session's active branch at the top level, and
under `history` as much of the branch before it as the hook asked for. `spec`
is the hook's declared input (`TurnEditInputSpec`, read from `hooks.input` by
`hook_next`).

### What a hook asks for

The manifest entry declares it; the runtime sends exactly that:

```json
{"event": "turnSettled", "handler": "review.qjs", "wait": true,
 "input": {"history": 5, "history_results": false, "max_bytes": 2000000}}
```

| Option | Values | Default | |
|---|---|---|---|
| `history` | `"none"`, a positive integer N, `"all"` | `"none"` | earlier turns sent with the finished one: none, the last N, or the whole active branch including any compaction summary |
| `history_results` | boolean | `true` | `false`: earlier turns' tool results come without their text |
| `max_bytes` | integer, 1 … 16777216 | 1048576 (1 MiB) | the most the whole input may be, in bytes of JSON text |

Every option is optional, and so is `input` itself: a hook that declares
nothing — `extensions/turn-review` is one — is sent the finished turn alone,
as before history existed. `extension_manifest_validate` refuses anything
else at promote, naming it: `input` not an object, a key other than these
three, `history` that is not `"none"`, `"all"` or an integer ≥ 1,
`history_results` that is not a boolean, `max_bytes` that is not an integer
from 1 to 16777216. The declaration is stored as given in `hooks.input` (NULL
when absent); the defaults are the runtime's and are filled in each time the
hook runs, so they are not frozen into old rows.

**The defaults, and why.** 1 MiB is about the most a script on the default
QuickJS heap (`js_heap_mb` 8) can parse — the parsed value costs several times
its text. A script that asks for more history should raise both. The ceiling,
16 MiB (`TURN_EDIT_INPUT_MAX_BYTES`), is the largest value the hook vehicle
carries (`RUNTOOL_ARG_FD_MAX`; `settle.c` asserts they are equal): an input
past it could not be delivered at all.

### The rules

1. **The finished turn is always sent whole**, every result with its text. If
   it alone does not fit in `max_bytes`, nothing is built: `turn_edit_input`
   returns NULL with `too_big` set, and the hook is "not started" — one log
   line (`outcome=not_started reason=the finished turn alone is larger than
   the hook's max_bytes (N)`), the session never held, and not counted as a
   failure of the script (the same as any hook that cannot be started,
   [hooks.md](hooks.md#the-hold-always-ends)).
2. **`history`** selects the earlier part of the branch. `"none"` sends `[]`,
   and the statement reads the newest turn's entries and no others — the cost
   of building the input is that of one turn whatever the session's length. N
   is the last N turns, oldest first, and any compaction summary with fewer
   than N turns between it and the finished turn — so a summary older than all
   N is not sent. `"all"` is the
   whole branch: what compaction left of the first turn, the summary, every
   turn since.
3. **`history_results: false`** keeps every earlier call, its `args`, and its
   `result` object, but the `result` has no `text` key —
   `{"entry_id":905,"tokens":2900}` — so a script can still name the entry or
   weigh it. A call with no result still has `result: null`. The finished
   turn's results are never cut.
4. **`max_bytes`** caps the whole input. History that does not fit loses its
   oldest turns first, whole turns only — never part of a turn, never a gap
   in the middle. A compaction summary is kept in preference to every turn
   older than it (in practice, the opening of the first turn that compaction
   keeps), and goes before any turn newer than it.
5. **The input says what it was given**: `input` echoes the resolved options
   (`{"history":"none"|N|"all","history_results":…,"max_bytes":…}`), and
   `history_omitted` counts the requested earlier turns left out for size (0
   when nothing was cut; a summary left out is not counted — it goes only with
   every turn older than it). A script can therefore tell a session's first
   turn (`history: []`, `input.history` not `"none"`, `history_omitted: 0`)
   from "not asked for" (`input.history: "none"`) and from "cut for size"
   (`history_omitted > 0`).

**What `max_bytes` bounds, precisely**: the length in bytes of the JSON text
handed to the script — the returned string is never longer. Which earlier
turns fit is decided first from integers alone, before any text is read: each
entry is priced at its stored text (`entries.content_bytes`) plus a fixed 128
bytes for the keys and ids around it, the head at 512 bytes, and an earlier
result whose text is not sent at the 128 alone. That estimate is close for
ordinary text but runs short where JSON escaping grows text — quotes and
newlines double, control characters become six bytes. So the built document's
real length is checked: if it is over, the statement runs again with the
overshoot held back from history (more of the oldest goes), and a third and
last time with no history at all; if even the turn alone is over, the hook is
not started (rule 1). Each run reads text only for the entries it sends; in
the ordinary case there is one run.

### Shape

```json
{"event":"turnSettled","session_id":12,"agent":"alavanca",
 "input":{"history":"all","history_results":true,"max_bytes":1048576},
 "turn_id":37,
 "tokens":{"turn":9120,"tools":7400},
 "user_messages":[{"entry_id":900,"text":"…"}],
 "notes":[{"entry_id":930,"text":"…","before_iteration":412}],
 "iterations":[
   {"iteration_id":411,"entry_id":901,"text":"Let me look at the logs.",
    "calls":[{"call_id":"c1","entry_id":903,"tool":"shell_exec",
              "args":{"command":"…"},"protected":false,"is_error":false,
              "result":{"entry_id":905,"text":"…","tokens":2900}}]}],
 "final":{"entry_id":960,"iteration_id":413,"text":"…"},
 "history":[
   {"turn_id":1,"tokens":{"turn":40,"tools":0},
    "user_messages":[{"entry_id":3,"text":"…"}],"notes":[],"iterations":[],
    "final":null},
   {"summary":{"entry_id":880,"text":"…","tokens":1200}},
   {"turn_id":36,"tokens":{…},"user_messages":[…],"notes":[…],
    "iterations":[…],"final":{"entry_id":899,"iteration_id":410,"text":"…"}}],
 "history_omitted":0}
```

- `input` — the options this input was built with (above).
- `tokens` — sums of `entries.token_estimate`: the whole turn, and its
  `tool_call` + `tool_result` entries. These are the runtime's estimates
  (chars/3), not the provider's count, which can be higher; thresholds a
  script compares them with are in the same estimate units.
- `user_messages` — the turn's `user_message` entries, in order.
- `notes` — `system` entries already in the turn (runtime notices, notes an
  earlier hook left), with the id of the iteration each precedes (the
  answer's, for a note right before the answer).
- `iterations` — every iteration but the last, in order. `entry_id` is the
  iteration's first row on the branch (its `reasoning` row when it has one) —
  what a note's `before` names. `text` is its assistant text (`""` when none).
  `calls` are in call order. `args` is the parsed object, or the stored string
  when it is not JSON. `result` is null for a call with no result; `text` is
  the stored content verbatim. A result is paired with its call by call id
  within the turn, not by `iteration_id`: a sub-agent's answer or an approval
  notice is appended under an iteration id of its own.
- `final` — the turn's last assistant message. It is never edited.
- `history` — the earlier part of the branch that was asked for and fits,
  oldest first ([below](#history)). Always an array: `[]` when none was asked
  for, on a session's first turn, or when none fits.
- `history_omitted` — earlier turns asked for and left out for size.

### History

It is for reading only — an edit names calls and entries of the newest turn
and nothing else.

`history` is the active branch as the agent now has it — the entries the next
request is built from — not the session as first written:

- **An earlier turn** is an object with the newest turn's own fields:
  `turn_id`, `tokens`, `user_messages`, `notes`, `iterations`, `final`. One
  serializer writes both (`SQL_TURN_FIELDS`), so a field means the same thing
  in either place. `final` is `null` for a turn that did not end with an
  assistant message, or of which only the opening is left.
- **A compaction summary** is `{"summary":{"entry_id","text","tokens"}}`, at
  the place it stands on the branch. After an ordinary compaction that is
  second: the branch keeps its first entry, so `history[0]` is what is left
  of the first turn (its opening user message, `final: null`) — a turn, for
  counting N and for `history_omitted` — then the summary, then whole turns.
  What a summary replaced is not in `history`. A summary written by
  `session_condense` stands between the turns on either side of the range it
  replaced.
- **An edited turn** shows as edited: dropped calls are absent, a replaced
  result has the replacement's text and entry id, a hook's note is in
  `notes`. The originals are in the table, not on the branch.
- An assistant entry that stopped on an error is left out of an earlier turn,
  as it is left out of every request (`context_plan`), and costs nothing
  against `max_bytes`. The newest turn is reported in full.
- `protected` is computed for an earlier turn's calls as for the newest
  turn's. It says what kind of call it was; no edit can reach it.

Two ways `history: "all"` can be more than the next request carries, both
because the request's cut depends on the model it is built for and this input
does not: the branch is not trimmed to the context budget (with compaction
off or failing, the request is a sliding window over the tail; here only
`max_bytes` trims), and the leading system prompt and the per-turn context
block are not in it — they are not entries.

Call ids are scoped to their turn. Providers reuse them across turns, so a
call's `result` is looked up among its own turn's entries, and an edit's
`call_id` among the newest turn's (`turn_load`); a `call_id` or `before` taken
from `history` is refused like any other id that is not the turn's.

**Building it.** One statement, two passes over the branch: a recursive walk
from the leaf over integer columns (ids, parents, `turn_id`, `content_bytes`)
numbers the units — the newest turn, each earlier turn, each summary —
stops at the first unit past the N asked for, prices them and decides which
fit; then text is read for the chosen entries only. Building the document
costs about five times its size in memory for a moment, so `max_bytes` also
bounds what the daemon spends.

### Protected calls

`protected: true` means the call cannot be dropped. Its result's text can
still be replaced.

- The tool is not marked droppable. `tools.droppable` is 1 only for tools
  whose purpose is to look something up; a call made for its effect — a file
  written, a message sent, an agent launched, a memory note, a config request,
  a schedule, a secret saved — is the record that the effect happened.
  Built-in tools declare it in their C recipe (`ToolRecipe.droppable`, synced
  by `tools_sync_to_db`); an extension tool declares `"droppable": true` in its
  manifest. A tool with no `tools` row is protected. `check_session` is
  protected: on an idle child it returns the child's answer and advances the
  delivery cursor, so its result can be the only copy.
- The call passed `save_secret`: whatever the tool, that call stored a
  credential.
- The call's `tool_calls` row has a status other than `done` (a background
  job).

## Reply

`turn_edit_apply(db, session_id, reply_json, source_label, &err)`.
`source_label` is `hook:<extension>`.

```json
{"edits":[
   {"op":"drop","call_id":"c4"},
   {"op":"replace","call_id":"c1","text":"…"},
   {"op":"note","text":"…","before":901}],
 "record":{"…":"…"}}
```

| op | fields | does |
|----|--------|------|
| `drop` | `call_id` | takes the call and its result off the branch |
| `replace` | `call_id`, `text` | puts a new result with this text in the old one's place |
| `note` | `text`, `before?` | inserts a system note before an iteration; default: before the answer's iteration |

`record` is an optional object the runtime stores untouched with its own
record of the edit (below). It is never sent to the model.

Returns `TURN_EDIT_APPLIED`, `TURN_EDIT_NOTHING`, `TURN_EDIT_REJECTED` or
`TURN_EDIT_ERROR`. Only `APPLIED` writes anything; `REJECTED` and `ERROR` set
a one-line reason.

**Nothing to do** — `{}`, `{"edits":[]}`, or a record with no edits (a record
describes edits). Shape checks run first, so a malformed record with no edits
is `REJECTED`, not `NOTHING`.

**Rejected whole** — one failed check refuses the entire reply:

- shape (reason `edits[N] …` for an edit): not a JSON object; a top-level key
  other than `edits` and `record`, or one given twice; `edits` not an array;
  `record` not an object, or larger than 16 KB
  (`TURN_EDIT_RECORD_MAX_BYTES`); an edit that is not an object, has an
  unknown `op`, names a field twice, has a field its op does not take, lacks
  a required field, or has one of the wrong type (`call_id` a string, `text`
  a non-empty string, `before` an integer);
- calls (reason `drop: …` / `replace: …`): a call id that is not a
  `tool_call` of this turn on the branch, or that names more than one; one
  call named by two edits; a call whose `tool_calls` row is not `done`;
  dropping a protected call; replacing on a call with no result, or with more
  than one;
- notes (reason `note: …`): `before` that is not the first row of an
  iteration of this turn (the `entry_id` of an `iterations` item, or of the
  answer's iteration), or that names an iteration this reply empties;
- text (reason `edits[N]: …`): invisible characters are stripped first, and
  text that is empty after that is refused; then the text goes through
  `tool_result_postprocess` with a fresh `secrets_snapshot` — stored secret
  values and the scanner's rules, the chain `inbox_insert_scanned` runs. A
  hit refuses the reply; text is never stored masked;
- the turn is not finished: the session's leaf is not an
  `assistant_message`, or a call of the turn is still `pending` or `running`.

`ERROR` is a failure on our side: a DB error, or an answer entry whose `data`
is not valid JSON (the record cannot be appended without overwriting it, so
nothing is written).

The reply's shape and text are checked without a transaction. Everything that
reads the turn runs inside one `BEGIN IMMEDIATE`, so what is checked is what
is edited; the function refuses to run inside a caller's transaction.

## What is written

All in that one transaction.

- **Drop.** The `tool_call` entry and its `tool_result` leave the branch. An
  iteration left with no call leaves whole — assistant text and reasoning
  included. One that keeps some of its calls loses its `reasoning` entry: a
  provider's reasoning blob is bound to the full set of calls it was produced
  with and must not be replayed beside a subset.
- **Replace.** A new `tool_result` takes the original's place, carrying its
  `iteration_id`, `turn_id`, `tool_call_id`, `tool_name`, `is_error`,
  `network_hosts` and `fence_id` — so a replaced network result keeps its
  untrusted-content fence on the wire. Any length is applied.
- **Note.** An ordinary `system` entry: role 0, `iteration_id` 0, the turn's
  `turn_id`, `data` = `{"source":"hook:<extension>"}`. Several notes at one
  place land in edit order.

The turn's chain is rebuilt front to back: each entry that stays hangs off the
entry that now precedes it. A re-pointed entry keeps the parent it was written
under in `original_parent_id` — the first one, if it is re-pointed again; an
entry that leaves the branch is not modified. The answer keeps its row and
stays `sessions.leaf_id` (inserts move the leaf through `entries_leaf_ai`; it
is put back before commit). New entries carry the turn's `turn_id` explicitly,
so `entries_turn_ai` does not open a turn; the next user message opens a new
one under the answer as usual.

Before `COMMIT` the turn is walked again from the leaf: it must be one chain of
exactly the expected entries, hanging off the same predecessor. Otherwise the
transaction is rolled back and the call returns `ERROR`.

Not changed: `tool_calls` rows, usage and cost columns, every entry of earlier
turns.

### The record

One object is appended to `$.turn_edits` in the `data` of the turn's final
assistant entry (other keys there are kept). A turn may be edited more than
once — by several hooks in a row — and each apply appends one element.

```json
{"by":"hook:review","at":1763300000,
 "left_branch":[903,905],
 "replaced":[{"original":910,"replacement":971}],
 "notes":[972],
 "tokens":{"before":9120,"after":3100},
 "record":{"…":"the script's own, when it sent one"}}
```

- `left_branch` — every entry that left the branch, in chain order, replaced
  originals included.
- `replaced`, `notes` — in edit order.
- `tokens` — the turn's size before and after this apply, in estimate units
  (`entries.token_estimate`). The compaction check that follows the apply
  reads `before − after` back as the shrink since the provider last counted
  ([memory.md](memory.md#context-window-management)).

## On the wire

A note is sent like any mid-conversation system entry — user-role text — but
labelled by its source: `[note from extension <ext>] <text>`. Every other
`system` entry keeps `[system] <text>` (`SQL_SYSTEM_NOTE_PREFIX`,
`src/llm_payload.c`). A hook's note is typically written by a model that has
just read untrusted tool output; `[system]` would lend that text the
runtime's own authority. A `system` entry whose `data` is not valid JSON is
sent as `[system]`.
