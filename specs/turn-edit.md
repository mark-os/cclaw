# Turn edit — what a turnSettled script sees and may change

`src/turn_edit.c`. Two functions: `turn_edit_input` builds the finished turn
as plain data for a [turnSettled](hooks.md) script, and `turn_edit_apply`
checks the script's reply and edits the turn. They run no script and change
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

`turn_edit_input(db, session_id)` returns the newest turn on the active branch
as JSON, or NULL when the session's leaf is not an `assistant_message` (no
finished turn). It is the script's `args`.

```json
{"event":"turnSettled","session_id":12,"agent":"alavanca","turn_id":37,
 "tokens":{"turn":9120,"tools":7400},
 "user_messages":[{"entry_id":900,"text":"…"}],
 "notes":[{"entry_id":930,"text":"…","before_iteration":412}],
 "iterations":[
   {"iteration_id":411,"entry_id":901,"text":"Let me look at the logs.",
    "calls":[{"call_id":"c1","entry_id":903,"tool":"shell_exec",
              "args":{"command":"…"},"protected":false,"is_error":false,
              "result":{"entry_id":905,"text":"…","tokens":2900}}]}],
 "final":{"entry_id":960,"iteration_id":413,"text":"…"}}
```

- `tokens` — sums of `entries.token_estimate`: the whole turn, and its
  `tool_call` + `tool_result` entries.
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
- `tokens` — the turn's size before and after this apply.

## On the wire

A note is sent like any mid-conversation system entry — user-role text — but
labelled by its source: `[note from extension <ext>] <text>`. Every other
`system` entry keeps `[system] <text>` (`SQL_SYSTEM_NOTE_PREFIX`,
`src/llm_payload.c`). A hook's note is typically written by a model that has
just read untrusted tool output; `[system]` would lend that text the
runtime's own authority. A `system` entry whose `data` is not valid JSON is
sent as `[system]`.
