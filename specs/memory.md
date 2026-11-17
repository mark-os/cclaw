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

On `/admin` ([explorer.md](explorer.md)) every block has one page, reached from
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
(`entries.tokens`, by `usage_attribute` in `db_response.c`). Our character
estimate (`entries.token_estimate`, chars/3 + 4) is the size of an entry no
report has measured yet. Everything that sizes a context — the cut, the
overflow probe, the compaction trigger, a hook's `tokens` — reads
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
  same model, no compaction and no turn edit between them — and when what
  is left for the appended entries is within sight of their estimate
  (between a third and three times it, plus framing): a moved cut or a
  changed system prompt shows up there. Otherwise nothing is written and
  those entries keep their estimate. An entry is measured once or not at
  all; an errored response is never sent, is no report and takes no share.
- **A turn edit** needs nothing measured again: a dropped call leaves the
  branch with its count, and a replaced result keeps its measure scaled to
  the new length (a short one, mostly framing, goes back to the estimate).
- The **head** (tools, system prompt, user context block, JSON framing) is
  not an entry. It is the larger of its estimate (`context_head_estimate`)
  and one report's `usage_in` less the entries that request was sent with —
  the newest report written after every compaction whose turn has not been
  edited since. When that request had been cut it reads low and the estimate
  stands. Because it is anchored on a whole reported request, head + entries
  is that request plus what came after it, however the shares fell.

Budgets and windows are in the provider's tokens, and so are the sizes
compared against them at every site below. With no report at all every size
is the estimate and every site reads as it always has.

- Load ≤ `context_threshold` × context_window tokens of most recent turns:
  the cut keeps the newest turns whose measured size fits budget − head
  (`context_plan`)
- Never cut mid-tool-call; the cut snaps **backwards to the start of the turn it
  lands in** (`entries.turn_id`, `plan_find_cut` in `context.c`) — not to "the
  previous user message", which would split a turn that opened with several
  inbox-drained user entries, and not forwards, which could drop the very turn
  being answered. Keeping a whole turn can overshoot the budget by one turn.
- Cutoff notice prepended when truncated
- Mid-turn overflow probe (`context_turn_overflows`, run before each
  iteration ≥ 1): the cut cannot split the current turn, so once the turn's
  size — each entry at its stored count, the results since the newest
  request by estimate — exceeds the window less the head,
  the turn stops with the context-budget notice instead of sending a request
  the provider would refuse (E5).
- Compaction trigger (`session_needs_compaction`, after each turn): the
  context size is the larger of head + the measured branch and the sent
  body / 3 less what a turn edit removed since (estimate units, from the
  answer's `$.turn_edits` record); it fires above `context_threshold` ×
  window less the just-finished turn's measured size. Without an edit since
  the newest request this is at least that request's count plus what came
  after it, so never lower than the plain floors; after one, the edited turn
  is no longer read at its pre-edit count, so the shrink is seen at the
  check that follows the edit.
- Compaction: summarize old entries, reparent tree. Its keep boundary
  (`context_compaction_keep_from`) keeps `compaction_target` × window of
  measured size and snaps the same way, so a summary never lands inside a
  turn; a branch that is one giant turn compacts to nothing.
- Turn edit: a turnSettled hook script can edit the turn that just finished
  before the next one starts — tool calls dropped, result text replaced,
  notes added — with the same re-pointing compaction uses, inside one turn.
  See [turn-edit.md](turn-edit.md).
- **Mechanical state coda** (E2): the compaction entry is the model's prose
  plus a `--- carried state ---` section built parent-side *after* the
  compaction call returns (`compaction_state_coda`, `llm_proc.c`) — open
  approvals (pending and approved-but-unconsumed tickets), running sub-agent
  sessions, armed cron one-shots, queried live in one SQL pass. It is never
  sent to the compaction model, so the summary cannot drop or paraphrase it.
  Still exactly one role-4 entry; empty state appends nothing. A failed
  compaction writes no entry at all (no coda-only rows) — the failure counter
  in `compaction_record_outcome` records it. The read-time sliding window gets
  no coda: that state is already live in the user context block and
  `check_session`.

## Letta Patterns NOT Adopted

| Letta Feature | CClaw Decision | Reason |
|---------------|---------------|--------|
| Shared blocks across agents | Implemented as explicit attachments (`agent_memory_blocks`); no global block | A NULL-agent block would silently apply to every future agent |
| Sleeptime background agent | Not implemented | Adds complexity; agent can manage own memory |
| Stateless mode | Not needed | Sessions are cheap, tree structure handles branching |
| PostgreSQL + pgvector | SQLite single-file | No server dependency, trivial deployment |
| 100K char block limit | 5K default | Smaller context windows, cheaper models |
