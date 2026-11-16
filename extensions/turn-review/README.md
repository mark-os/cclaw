# turn-review

A `turnSettled` hook that tidies a turn after it ends. Once the reply has
been delivered, a cheap model reads the finished turn and picks what later
turns no longer need:

- **drop** a tool call that was not on the path to the answer (a search that
  found nothing, a dead end, a failed attempt that was redone). The call and
  its result leave the conversation together.
- **trim** a long result to the line ranges that mattered.
- **note** — one or two sentences saying what the removed material
  established.

Everything after that turn is cheaper: the turn is sent again with every
later request, so 12,000 tokens of build log that came down to five lines
stop being paid for on each one.

The model only selects. `review.qjs` builds every excerpt from the original
text, so a trimmed result contains the tool's own lines and nothing else:

```
[turn-review: this result was shortened after the turn ended. You read all 343 lines before you answered; the full text is entries.id=51]
[lines 1-187 omitted, see entries.id=51]
src/sched.c:412:17: error: 'struct task' has no member named 'deadline_ns'; did you mean 'deadline_us'?
  412 |     if (t->deadline_ns < now) reap(t);
make[2]: *** [Makefile:88: build/sched.o] Error 1
[lines 191-341 omitted, see entries.id=51]
make[1]: *** [Makefile:31: all] Error 2
make: *** [Makefile:12: default] Error 2
```

Nothing is deleted. Originals stay in the database, and the markers and the
note name the entries that hold them, so an agent with `db_query` can read
one back (`SELECT content FROM entries WHERE id=51`). The runtime refuses
anything a script should not do — dropping a call that was made for its
effect (a file written, a message sent), touching the answer or the user's
messages — and applies a review whole or not at all
([specs/turn-edit.md](../../specs/turn-edit.md)).

It is the reference example for hooks: one file of about 240 lines, with the
settings at the top. Its manifest entry declares no `input`, so it is sent
the finished turn alone, up to 1 MiB — the runtime default. A hook that wants
the conversation before the turn says so in its entry (`"input": {"history":
5}`, or `"all"`; see [specs/turn-edit.md](../../specs/turn-edit.md#input));
this one reviews one turn and does not need it.

## Attach it

It is never on by default. Put the bundle in the agent's workspace and have
the agent promote it — promotion is an approval, like any extension:

```sh
cp -r extensions/turn-review ~/.cclaw/agents/<Agent>/workspace/extensions/
```

then tell the agent: *promote the extension draft turn-review*. The approval
reads `promote 'turn-review': adds 0 tools, 1 hooks, … 2 config keys`, then
`- hook turnSettled review.qjs: runs after every turn, reads it, and may drop
tool calls, replace results and add notes; the session waits for it. It is
sent the finished turn`. From
the agent's next turn the hook runs; no restart. For a second agent, have the
owner `extension_publish` it and the other agent `extension_attach` it.

Only top-level sessions are reviewed, never a sub-agent's.

## Give it a utility model

The script asks for `LLM(…, {purpose: "utility"})` and names no model. Put a
small, fast model on the agent's **utility** list — the explorer's agent page
has it under Models, or:

```sql
-- cclaw sqlite3 --write ~/.cclaw/cclaw.db "…"
INSERT INTO agent_models(agent_name, purpose, model_id, pos)
VALUES ('<Agent>', 'utility', '<a model id from the models table>', 0);
```

With no utility list the review runs on the agent's chat models, which works
and costs more.

## Settings

| Key | Default | |
|---|---|---|
| `turn-review.min_tool_tokens` | 4000 | A turn with less tool traffic than this is left alone, and the model is not asked. |
| `turn-review.max_turn_tokens` | 60000 | A turn bigger than this is not reviewed; ordinary compaction deals with it. |
| `turn_settled_timeout` | 60 | The runtime's deadline for any hook script, in seconds. The review asks its model with a 45 s limit. |

Token counts are the runtime's estimates. The prompt, the note length and the
marker wording are constants at the top of `review.qjs`: edit the draft and
promote again.

## See what it did

- **The dashboard**: a session's page marks the turn "edited", shows it as the
  agent now sees it, and under "What was taken out" the dropped calls and the
  original text of each shortened result.
- **The log**, one line per turn: `hook turnSettled ext=turn-review
  outcome=applied` (or `nothing`, `rejected`, `failed`, with a reason).
- **The record** on the turn's final assistant entry — what left, what
  replaced what, sizes before and after, and the script's own list of calls:

  ```sql
  SELECT id, json(data -> '$.turn_edits') FROM entries
   WHERE data -> '$.turn_edits' IS NOT NULL ORDER BY id DESC LIMIT 5;
  ```

- **The reviewer's answers**: `cclaw resp list` shows the calls labelled
  `hook:turn-review`; `cclaw resp <id>` shows what the model said, and
  `cclaw resp <id> req` what it was shown.
- **An original**: `SELECT content FROM entries WHERE id=<id from a marker>`.

To stop it for one agent:

```sql
UPDATE agent_extensions SET enabled=0
 WHERE agent_name='<Agent>' AND extension_name='turn-review';
```

## What to expect

- The session is held while the review runs, a few seconds with a small
  model. Messages that arrive meanwhile wait in the inbox.
- The reviewer reads the whole turn with every result line numbered, which
  is roughly twice the turn's size in input tokens, once. What it removes is
  saved on every later request of the session.
- A model answer that cannot be read, names a call that does not exist, asks
  to drop a protected call, or gives a line range outside the result changes
  nothing: the script returns no edits. A model that cannot be reached is a
  failed run; after three in a row the runtime tells the operator once. The
  turn is untouched either way.
- The agent is told when a result was shortened, and that it had read all of
  it. Without that it tends to distrust its own earlier answer.
- The note is written by a model that has just read tool output, so the
  runtime sends it as `[note from extension turn-review] …`, never as
  `[system]`.
