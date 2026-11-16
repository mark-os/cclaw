# Turn review — editing the turn that just finished

`src/turn_review.c`. Two functions: one builds what a reviewer is shown, the
other validates what it sends back and edits the turn. Nothing calls them yet;
they change no session state and run no reviewer.

A turn that has ended can carry material the rest of the session does not
need: a search that found nothing, 300 lines of log of which five mattered.
The edit takes that off the session's active branch, the way compaction does
for whole turns — rows are re-pointed, never deleted — so later requests do
not carry it.

## The reviewer's input

`turn_review_input(db, session_id)` returns the newest turn on the active
branch as JSON, or NULL when the session's leaf is not an assistant message.

```json
{"event":"turnSettled","session_id":12,"agent":"alavanca","turn_id":37,
 "user_messages":[{"entry_id":900,"text":"…"}],
 "iterations":[
   {"iteration_id":411,"text":"Let me look at the logs.",
    "calls":[{"call_id":"c1","tool":"shell_exec","args":{"command":"…"},
              "protected":false,"is_error":false,
              "result":{"entry_id":903,"lines":312,"tokens":2900,
                        "text":"1| …\n2| …"}}]}],
 "final":{"entry_id":960,"text":"…"},
 "tokens":{"turn":9120,"tool_traffic":7400},
 "limits":{"note_max_chars":600}}
```

- `user_messages` — the turn's `user_message` and `system` entries, in order.
- `iterations` — every iteration but the last, in order; `calls` in call
  order. `args` is the parsed object, or the stored string when it is not
  JSON. `result` is null for a call with no result on the branch. A result
  is paired with its call by call id within the turn, not by `iteration_id`:
  a sub-agent's answer or an approval notice is appended under an iteration
  id of its own.
- `result.text` — the stored content with every line prefixed `<n>| `.
  **A line** is the content split on `\n`; a final `\n` ends the last line
  rather than opening an empty one. `result.lines` is the count. The same rule
  is used when a trim is applied.
- `final` — the turn's last assistant message. It is never edited.
- `tokens` — sums of `entries.token_estimate`: the whole turn, and its
  `tool_call` + `tool_result` entries. `turn_review_measure()` returns the
  same two numbers without building the document.

### Protected calls

`protected: true` means the call cannot be dropped. It can still be trimmed.

- The tool is not marked droppable. `tools.droppable` is 1 only for tools
  whose purpose is to look something up; a call made for its effect — a file
  written, a message sent, an agent launched, a memory note, a config request,
  a schedule, a secret saved — is the record that the effect happened. Built-in
  tools declare it in their C recipe (`ToolRecipe.droppable`, synced to the
  table by `tools_sync_to_db`); an extension tool declares
  `"droppable": true` in its manifest and is protected when it does not. A
  tool with no `tools` row is protected.
- The call passed `save_secret`: whatever the tool, that call stored a
  credential.
- The call is not finished — its `tool_calls` row has a status other than
  `done` (a background job).

## The reply

`turn_review_apply(db, session_id, reply_json, reviewer_model, &err)`

```json
{"drop":["c4","c5"],
 "trim":[{"call_id":"c1","keep":[[120,160],[298,312]]}],
 "note":"…"}
```

Every key is optional. `keep` is a list of inclusive 1-based line ranges.

Returns `TURN_REVIEW_APPLIED`, `TURN_REVIEW_NOTHING` (the reply asked for no
change), `TURN_REVIEW_REJECTED` (a check below failed) or `TURN_REVIEW_ERROR`
(a failure of ours). Only `APPLIED` writes anything; the other two failures
set a one-line reason.

**Nothing to do.** `{}`, empty lists, an empty note. Also a trim that keeps
every line, and a trim whose excerpt would be no smaller than the result it
replaces (a marker line costs more than a short line): those are ignored, not
refused, and a reply made only of them is `NOTHING`.

**Rejected whole** — one failed check refuses the entire reply:

- not a JSON object; a key other than `drop`, `trim`, `note`; a key given
  twice; `drop` not an array of strings; a `trim` item that is not exactly
  `{call_id: string, keep: array}`; a range that is not a pair of JSON
  integers; `note` not a string;
- a call id that is not a `tool_call` of this turn on the branch, or names
  more than one; an id listed twice; an id in both `drop` and `trim`;
- dropping a protected call;
- dropping or trimming a call that is not finished;
- trimming a call with no result; an empty `keep` (that is a drop); a range
  outside `1 ≤ a ≤ b ≤ lines`; ranges not ascending or overlapping (adjacent
  ranges are fine);
- a note longer than 600 characters (`TURN_REVIEW_NOTE_MAX_CHARS`), or one in
  which the secret path finds anything: invisible characters are stripped,
  then the note goes through `tool_result_postprocess` with a fresh
  `secrets_snapshot` — stored secret values and the scanner's rules, the same
  chain `inbox_insert_scanned` runs. A hit refuses the reply; the note is
  never stored masked;
- the turn is not finished: the session's leaf is not an `assistant_message`,
  or a call of the turn is still `pending` or `running`;
- the turn already has a `turn_review` entry.

The reply's shape and the note are checked first, without a transaction.
Everything that reads the turn happens inside one `BEGIN IMMEDIATE`, so what
is checked is what is edited; the function refuses to run inside a caller's
transaction.

## What is written

All in that one transaction.

- **Drop.** The `tool_call` entry and its `tool_result` leave the branch.
  If the iteration keeps other calls, its `reasoning` entry leaves too, so no
  provider is replayed reasoning beside a subset of the calls it was produced
  with. If the iteration has no call left, all of it leaves — assistant
  message and reasoning included.
- **Trim.** A new `tool_result` entry takes the original's place. Its content
  is the kept lines in order, each omitted run — leading and trailing ones
  too — replaced by one line:

  ```
  [… 119 lines omitted; full output is entry 204 …]
  ```

  The text is `TURN_REVIEW_OMIT_FMT`, fixed in `src/turn_review.h`. The
  original row stays in the table, unchanged, which is what the marker points
  at.
- **Review entry.** One entry of type `turn_review` (role 0), placed before
  the turn's last iteration, holding the note and the record of what was done
  — see [schema.md](schema.md#entriesdata-on-a-turn_review-entry). With a
  non-empty note it is sent to the model as user text `[Turn review] <note>`;
  with none it is a record only and is not sent.

The turn's chain is rebuilt front to back: each entry that stays hangs off the
entry that now precedes it. A re-pointed entry keeps the parent it was written
under in `original_parent_id` (the first one, if it is ever re-pointed again);
an entry that leaves the branch is not modified at all. The last assistant
message keeps its row and stays `sessions.leaf_id` — the inserts move the leaf
through `entries_leaf_ai`, and it is put back before commit. New entries are
given the turn's `turn_id` explicitly, so `entries_turn_ai` does not open a
turn.

Before `COMMIT` the turn is walked again from the leaf: it must be a single
chain of exactly the expected entries, hanging off the same predecessor.
Otherwise the transaction is rolled back and the call returns `ERROR`.

Not changed: `tool_calls` rows (a dropped call's row still says `done` and
points at its original result), usage and cost columns, and every entry of
earlier turns.
