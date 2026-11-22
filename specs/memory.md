# Memory Model

Design reference. Derived from Letta's persistent agent memory architecture, adapted for CClaw.

## Three Tiers

| Tier | CClaw Implementation | Access Pattern |
|------|---------------------|----------------|
| Core (in-context) | `memory_blocks` + `memory_notes` in cclaw.db, attached to agents by `agent_memory_blocks` | Always in context, bounded by `char_limit`; `placement` picks system prompt or the per-turn user context block |
| Archival (long-term) | Not yet implemented: `passages` table + embeddings | Semantic search, unlimited |
| Recall (history) | `entries` table + FTS5 in cclaw.db | Keyword/semantic search over all past turns |

## Core Memory (blocks and notes)

Agent's working memory — always in context. Two words, used everywhere (tool
names, the rendered prompt, the dashboard, this spec):

- A **block** is a named container: `label`, `description`, `char_limit`,
  `placement`, `read_only`. It holds no text of its own.
- A **note** is one item of text in a block. It is addressed by its **id**,
  the number shown in `[brackets]`. An id is the note's row id: unique across
  the database, never reused, and never renumbered. (Notes used to be numbered
  1..N per block, and a delete renumbered the rest — so a second call in the
  same batch, written against the numbers the model had been shown, hit the
  wrong note.)

### Who sees a block: attachments

A block is attached to agents through `agent_memory_blocks`, and the
attachments are the only answer to "who has it". The agent that creates a
block is recorded in `memory_blocks.created_by` (v59; was `agent_name`) and is
attached read-write. `created_by` is history, not a permission: it grants
nothing, and detaching the creator leaves it with no access. A block attached
to more than one agent is a **shared** block: every holder sees the same
notes under the same label (the rendered block says `| shared` to every
holder that did not create it).

- There is **no global block**. "Every agent" is a list of attachments —
  visible, editable, and not silently extended to agents created later. (An
  earlier spec said `agent_name IS NULL` meant global; no query ever
  implemented it.)
- An attachment has its own `read_only` flag. Anyone but the creator is attached
  **read-only unless the operator says otherwise**: a writable shared block
  lets one agent write into another agent's prompt, which is a trust decision.
- Sharing is an operator act — the dashboard, or an agent definition's
  `memory_blocks[]` entry `{"attach": <block id>}`. A creating agent may share
  only a block it holds itself, and the `create_agent` approval names the
  block, who created it, and whether the new agent may write to it.
- An agent addresses a block by label, so it can never hold two attachments
  with the same label; `memory_block_attach` refuses the second.
- `memory_blocks.read_only` is the block-wide flag (no agent may change it);
  the effective flag for an agent is `block.read_only OR attachment.read_only`.

### Where a block renders: placement

- `context` (default for new blocks): rendered into the user context block (the `<RELEVANT_CONTEXT>` text, `llm_payload.c`), which rides at the **turn boundary** — a user message right before the first entry of the newest turn, never in the system prompt. The block (local wall clock with its timezone + recall + context-placement blocks + running sub-agents + open approvals — pending and approved-but-unconsumed, in `<open_approvals>`) is **materialized once at turn start** (`sessions.turn_context`, `llm_proc.c`) and reused verbatim by every tool-loop iteration, so both its content and position stay stable for the whole turn and each iteration extends the prompt-cache prefix instead of invalidating it. Live state is thus a turn-start snapshot; mid-turn changes reach the model as tool results.
- `system`: baked into the system prompt once per session (`agent_config.c`). Reserve for stable identity content — the system prompt stays byte-identical across turns, so provider prompt caching keeps working, but every edit invalidates the whole cached prefix on the next session render. The base Assistant's seeded `AGENT` and `USER` blocks use this placement. A block the agent edits often (running notes) belongs in `context`.

Placement is a property of the block, so a shared block renders in the same
place for every holder. The agent can change it with `memory_block_update`.

### What the agent sees

Both placements render a block the same way (one SQL expression,
`MEMORY_BLOCK_RENDER_SQL` in `db.h`, so they cannot drift):

```
### NOTES
description: Running notes: gotchas, things learned, work in progress.
usage: 2745/5000 chars | read-write | placement: context
[412 2026-08-27] extension_promote installs into the daemon's shared store…
[415 2026-11-13] channel_send action=list shows…
```

The date beside the id is when the note's text last changed (UTC). Without
it every note looks equally current, and one that went stale months ago is
indistinguishable from one written yesterday. It is a date, not an age, so
the rendered text stays byte-stable from one day to the next.

A block the agent holds but does not own adds `| shared` to the usage line.

### Tools

| Tool | Arguments | Notes |
|------|-----------|-------|
| `memory_block_create` | `label`, `description`, `placement?` | Recorded as `created_by` the caller, attached to it read-write |
| `memory_block_update` | `block`, `description?`, `placement?` | |
| `memory_note_add` | `block`, `text` | Refused if it would exceed `char_limit`; returns the new id |
| `memory_note_edit` | `block`, `notes: [{id, text}]` | Refused if it would exceed `char_limit`; an edit that does not lengthen the note always fits |
| `memory_note_delete` | `block`, `ids: [..]` | Other notes keep their ids |

All five refuse a read-only block. A note id that is not in the named block
changes nothing and is reported by id; a call that changed nothing is an
error, never a quiet success. Each note tool returns the block as it now
stands, with ids.

### Keeping memory clean

`char_limit` is the pressure that keeps a block clean, so it is made to bite:
a refused add or edit says the block is full, returns the block as it stands,
and tells the agent what to do — delete notes that are out of date or no
longer change what it does, merge ones that overlap, and keep facts that
change what it does next time rather than a record of what happened. The tool
descriptions and the seeded block descriptions say the same thing, and the
date on each note shows which ones are old. Nothing here judges relevance
itself; a separate agent that reviews memory is future work
([future.md](future.md)).

Agents cannot delete a block, change its `char_limit` or `read_only`, or
attach or detach it — those are the operator's, on the dashboard.

### Dashboard

On `/admin/data` ([explorer.md](explorer.md)) every block has one page, reached from
Home › Memory blocks or from an agent's Memory section: its fields (edit
description, placement, limit and the block-wide read-only flag; delete it,
after a confirm that says which agents it is detached from); the agents it is
attached to (attach — read-only by default — detach, switch an attachment
between read-only and writable; making a shared block writable asks first);
and its notes, oldest first, each editable and deletable, with a form to add
one. The memory blocks list and an agent's page can create a block (made by
the operator: `created_by` NULL; from an agent's page it is attached to that
agent read-write). The same label rule holds: an agent cannot hold two blocks
with the same label. The operator's edits are not bound by `read_only` or
`char_limit` — those constrain agents. A change to a `context` block reaches
the agent at its next turn; to a `system` block at its next session.

### Other properties

- **Seeded blocks**: at agent creation from the definition's `memory_blocks[]`
  (a `value` there becomes the block's first note).
- **Persistence**: blocks persist across all sessions of the agents holding them.

Key insight from Letta: memory management is a **tool-use problem**. The agent decides what to remember via tool calls, not system-level heuristics.

## Schema (in cclaw.db)

Three tables; the column reference is [schema.md](schema.md#memory_blocks).

- `memory_blocks` — the container: `created_by` (history: the agent that made
  it; NULL for a block the operator made), `label`, `description`,
  `char_limit`, `read_only`, `placement`. A block has no text column.
  `UNIQUE(created_by, label)` is kept from the old owner column: one agent
  cannot create two blocks with the same label, even after detaching one.
- `agent_memory_blocks` — attachments: `(agent_name, block_id)` plus the
  attachment's `read_only`.
- `memory_notes` — the content: `id`, `block_id`, `text`, `updated_at`. Shown oldest first
  (`ORDER BY id`); the render paths read only these rows.

## Recall (entries + FTS5)

All past messages searchable via `db_query` tool (read-only SQL) or auto-recall.

- FTS5 indexes `content` column directly (split-column schema)
- Agent can search own history: `SELECT content FROM entries_fts WHERE entries_fts MATCH ?`
- Context window manager loads recent turns; older entries accessible via search
- All in cclaw.db — agent reads via the `db_query` tool, not a direct file path

## Future: Archival (vector recall)

Not yet implemented:
- `passages` table (text, embedding BLOB, source_type, source_id) — in cclaw.db
- Hybrid search: FTS5 + vector similarity (RRF merge)
- Agent tool: `memory_search(query)` for explicit recall
- Auto-recall (implemented, FTS5-only today): top hits ride in the user context block (`<RELEVANT_CONTEXT>`) at the turn boundary — see Core Memory above

## Context Window Management

**Measure.** The provider reports usage on every response; that is the
measure, and it is written down once per entry when it arrives
(`entries.tokens`, by `usage_attribute` in `db_response.c`). Our byte
estimate (`entries.token_estimate`: 2.5 bytes per token plus 4 for the
entry's framing, `TOKEN_ESTIMATE` in `types.h`, the one definition C, SQL and
the bundled compaction script share) is the size of an entry no report has
measured yet. 2.5 reads ordinary text, code and JSON close to Claude's count
and high on Gemini, DeepSeek and GPT models; dense tool output (listings,
logs, random blobs) still reads low on every model, which is the report's
job to correct. Everything that sizes a context — the request cut,
the trigger, C's trim, a hook's `tokens` — reads
`COALESCE(tokens, token_estimate)` and adds; nothing is re-derived.

The rule, checked against the Anthropic, Gemini-native and OpenAI-compatible
wire formats: a request's `usage_in` is the previous request's, plus the
previous response's `usage_out`, plus whatever was appended in between, to
within a few tokens of framing. The one variable is reasoning, which costs
its full reported count while it is replayed and nothing once it is not —
encrypted or not.

- **What the model wrote** shares the response's `usage_out` as soon as it is
  ingested: the reasoning entry gets the reasoning tokens the provider
  reports (0 when there is nothing to replay), the message and each tool
  call share the rest by length.
- **What was appended** — tool results, user messages, notes — is measured
  by the next report: the growth in `usage_in` that the previous response's
  entries do not account for, shared by length among the entries appended
  since.
- **The user context block** is rebuilt at every turn boundary, so the
  first report of a turn also holds the change in its size. The new block is
  one of the sharers there, and the previous block's count
  (`sessions.turn_context_tokens`) is given back first — a block that grows
  is not booked on any entry.
- **Reasoning** stays at its count while it is still sent and counted.
  Inside a turn the previous response's reasoning is expected to be; across
  a boundary the Anthropic format drops the whole previous turn's
  (`llm_payload.c`), and some upstreams stop counting what they are sent.
  The expected account is tried first, the alternatives after it, and
  reasoning found dropped is set to 0. A model switch strips another
  model's reasoning from every request, and sets it to 0.
- **A difference is trusted** only when the two requests can be compared —
  same model, and no session edit since the previous report (a summary, or
  an entry carrying an edit's record, at least as new as that report) — and
  when what is left for the appended entries is within sight of their
  estimate (between a third and three times it, plus framing — the measured
  range of 1.05 to 6.9 bytes per token puts a real count at 0.36 to 2.4
  times the estimate): a moved cut
  or a changed system prompt shows up there. Otherwise nothing is written
  and those entries keep their estimate. An entry is measured once or not
  at all; an errored response is never sent, is no report and takes no
  share.
- **An edit** needs nothing measured again: what left the branch took its
  count with it, a stand-in keeps the original's measure scaled to its
  length (a short one, mostly framing, goes back to the estimate), and a
  note or summary an edit wrote keeps its estimate
  ([turn-edit.md](turn-edit.md#accounting-of-written-entries)).
- The **head** (tools, system prompt, user context block, JSON framing) is
  not an entry. When the newest report is usable, the head is its `usage_in`
  less the entries that request was sent with — larger or smaller than the
  estimate. Usable means written after every edit on the branch, and from a
  request that was sent whole: `llm_req` stamps `entries.request_cut` on a
  report whose request had been cut, since that request was not sent the
  entries subtracted. With no report, a report older than an edit, or a
  report from a cut request, the head is its estimate
  (`context_head_estimate`): the tools the request carries — the payload's
  own selection, grants intersected with the session's and the chat's tool
  filters, `launch_agent`'s live roster included (`llm_tools_sent`) — plus
  the system prompt and the session's user context block, at 2.5 bytes per
  token. Because a measured head is anchored on a whole reported request,
  head + entries is that request plus what came after it, however the shares
  fell.

Budgets and windows are in the provider's tokens, and so are the sizes
compared against them. With no report at all every size is the estimate.

Known limits:

- The first message of a session is never measured: there is no earlier
  report to difference against, so it keeps its estimate.
- When more than one account of reasoning fits what is left, the expected
  one wins. Only the split among entries can be off; the total is fixed by
  `usage_in`.
- Upstreams differ. DeepSeek through OpenRouter has counted replayed
  reasoning across a turn boundary in a live session and not in a direct
  probe, which is why reasoning is settled by trying accounts and not from a
  per-provider table.
- A cut that moves between two reports is caught by the plausibility band,
  not detected directly: `request_cut` says a request was cut, not where.

### The thresholds

Two numbers decide when a session is cut and how far, both on the routing
entry (`agent_models`), read off the agent's governing entry — its first
chat model that is not `disabled` (`context_limits`, `src/context.c`). A
smaller fallback further down the list is never trimmed for: it is sent the
request and may refuse it.

| Column | Unit | Empty means |
|---|---|---|
| `trim_threshold` | tokens | half the model's `context_window` |
| `prune_target` | tokens | half the threshold |
| `cache_lifetime` | seconds | unknown: no `CacheExpiring` (on an Anthropic chat entry it defaults to 3600 — [providers.md](providers.md#anthropic-messages)) |

`context_limits` holds the only copy of the two 50% defaults. The schema
refuses a `prune_target` at or above `trim_threshold` (a column check) and a
`trim_threshold` above the model's window (triggers `agent_models_window_bi`
/ `_bu`); nothing in C relies on either. Opting out of trimming is
`trim_threshold` set to the window. Set through the config document
(`agent.models`, [config-doc.md](config-doc.md)) or the explorer's model
list.

### The request cut

`context_plan` keeps the newest turns whose measured size fits the budget
less the head. The budget is `trim_threshold`, or `max_history_tokens` when
that is set. Between iterations the trim has already brought the session
under the threshold, so in practice the cut acts only on a turn's first
request, when the messages that opened it pushed the session over; the trim
at the end of that iteration makes the stored branch agree.

- The cut never splits a turn; it snaps **backwards to the start of the
  turn it lands in** (`entries.turn_id`, `plan_find_cut`) — not to "the
  previous user message", which would split a turn that opened with several
  inbox-drained user entries, and not forwards, which could drop the very
  turn being answered. Keeping a whole turn can overshoot the budget by one
  turn.
- A cutoff notice is prepended when the request was cut.

### Compaction

Compaction is a hook. At the end of every iteration — between two
iterations of a running turn as at turn end — a session over its
`trim_threshold` fires `ContextThreshold` ([hooks.md](hooks.md)); the
bundled extension `compaction`, attached to every agent, answers it by
summarising the oldest part of the session with the agent's own model, down
to `prune_target`: whole earlier turns first, then, if that is not enough,
the oldest iterations of the running turn. The actions already taken (every
protected call) are listed under the summary by the script, from the calls
themselves. An idle session above `prune_target` whose prompt cache is about
to lapse fires `CacheExpiring`, on which the same extension summarises a
top-level session down to half its size, or to the prune target if that is
lower. See
`extensions/compaction/README.md`.

What C keeps is the mechanism and one backstop:

- **The C check.** After the hooks, a session still over `trim_threshold`
  is trimmed by C to `prune_target` — oldest turns, then oldest iterations
  of the running turn, one note where they went, no model call — and a turn
  that still does not fit is stopped
  ([turn-edit.md](turn-edit.md#cs-trim)). This is all an agent gets with the
  extension switched off, disabled after three failures, or replaced by a
  script that under-trims.
- **No idle trim.** C never trims an idle session; `CacheExpiring` is a
  hook only.
- **One vocabulary.** A script's edits, C's trim and the agent's own
  `session_condense` (a summary of a range of its past turns, which it
  writes itself) all go through `turn_edit_apply`'s checks and writing: a
  call never leaves without its result, an unfinished call never leaves,
  originals are never deleted, every edit is recorded.

### The mid-turn rule

Three kinds of writes reach `entries` mid-turn:

1. LLM output.
2. Tool results answering this turn's calls.
3. A session edit applied between iterations, by a hook or C's trim.

Everything else (channel messages, cron fires, sub-agent and background
results) queues in the inbox and enters at a turn boundary as user entries.
A hook holds the session while it runs, so nothing is appended under it.

| The rule bought | Now |
|---|---|
| provider adjacency legality | kept by the pairing check: a call and its result leave together, and an iteration left with no call leaves whole |
| turn atomicity | kept: an edit is one transaction, and the inbox still enters only at a turn boundary |
| a byte-stable prefix for prompt caching | broken at the edit point, on purpose. The next request pays full price from the earliest changed entry, and Anthropic thinking after that point is not replayed. The threshold is the only guard against doing it often |

### Inbox items

An inbox item larger than 64 KB (`RUNTOOL_RESULT_MAX`) is cut when it
becomes a user entry: the entry keeps the first 64 KB (at a character
boundary) and a line saying where the full text is,
`<workspace>/.tool_results/<session>/inbox-<id>.out` — the same home as a
spilled tool result, readable by the agent's file tools under every sandbox
profile (`src/inbox_cap.c`). The text was already through the secret
scanner at insert. If the file cannot be written the entry says so.
Attachments and images are not touched.

## Letta Patterns NOT Adopted

| Letta Feature | CClaw Decision | Reason |
|---------------|---------------|--------|
| Shared blocks across agents | Implemented as explicit attachments (`agent_memory_blocks`); no global block | A NULL-agent block would silently apply to every future agent |
| Sleeptime background agent | Not implemented | Adds complexity; agent can manage own memory |
| Stateless mode | Not needed | Sessions are cheap, tree structure handles branching |
| PostgreSQL + pgvector | SQLite single-file | No server dependency, trivial deployment |
| 100K char block limit | 5K default | Smaller context windows, cheaper models |
