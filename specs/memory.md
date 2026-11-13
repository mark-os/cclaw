# Memory Model

Design reference. Derived from Letta's persistent agent memory architecture, adapted for CClaw.

## Three Tiers

| Tier | CClaw Implementation | Access Pattern |
|------|---------------------|----------------|
| Core (in-context) | `memory_blocks` + `memory_notes` in cclaw.db, attached to agents by `agent_memory_blocks` | Always in context, bounded by `char_limit`; `placement` picks system prompt or per-turn context block |
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

A block is attached to agents through `agent_memory_blocks`. The agent that
creates a block is its **owner** (`memory_blocks.agent_name`) and is attached
read-write. A block attached to more than one agent is a **shared** block:
every holder sees the same notes under the same label.

- There is **no global block**. "Every agent" is a list of attachments —
  visible, editable, and not silently extended to agents created later. (An
  earlier spec said `agent_name IS NULL` meant global; no query ever
  implemented it.)
- An attachment has its own `read_only` flag. A non-owner is attached
  **read-only unless the operator says otherwise**: a writable shared block
  lets one agent write into another agent's prompt, which is a trust decision.
- Sharing is an operator act — the dashboard, or an agent definition's
  `memory_blocks[]` entry `{"attach": <block id>}`. A creating agent may share
  only a block it holds itself, and the `create_agent` approval names the
  block, its owner, and whether the new agent may write to it.
- An agent addresses a block by label, so it can never hold two attachments
  with the same label; `memory_block_attach` refuses the second.
- `memory_blocks.read_only` is the block-wide flag (no agent may change it);
  the effective flag for an agent is `block.read_only OR attachment.read_only`.

### Where a block renders: placement

- `context` (default for new blocks): rendered into the `<RELEVANT_CONTEXT>` block (`llm_payload.c`), which rides at the **turn boundary** — a user message right before the newest user_message, never in the system prompt. The block (local wall clock with its timezone + recall + context-placement blocks + running sub-agents + open approvals — pending and approved-but-unconsumed, in `<open_approvals>`) is **materialized once at turn start** (`sessions.turn_context`, `llm_proc.c`) and reused verbatim by every tool-loop iteration, so both its content and position stay stable for the whole turn and each iteration extends the prompt-cache prefix instead of invalidating it. Live state is thus a turn-start snapshot; mid-turn changes reach the model as tool results.
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
| `memory_block_create` | `label`, `description`, `placement?` | The caller becomes the owner |
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

`/admin` has a **Memory** section: every block with its owner, usage,
description, placement, limit and read-only flag; the agents it is attached to
(attach, detach, switch read-only/read-write); and its notes, each editable
and deletable, with a form to add one. The operator's edits are not bound by
`read_only` or `char_limit` — those constrain agents. A change to a `context`
block reaches the agent at its next turn; to a `system` block at its next
session.

### Other properties

- **Seeded blocks**: at agent creation from the definition's `memory_blocks[]`
  (a `value` there becomes the block's first note).
- **Persistence**: blocks persist across all sessions of the agents holding them.

Key insight from Letta: memory management is a **tool-use problem**. The agent decides what to remember via tool calls, not system-level heuristics.

## Schema (in cclaw.db)

Three tables; the column reference is [schema.md](schema.md#memory_blocks).

- `memory_blocks` — the container: `agent_name` (owner; NULL for a block the
  operator made), `label`, `description`, `char_limit`, `read_only`,
  `placement`. A block has no text column.
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
- Auto-recall (implemented, FTS5-only today): top hits ride in the `<RELEVANT_CONTEXT>` block at the turn boundary — see Core Memory above

## Context Window Management

- Load ≤ `context_threshold` × context_window tokens of most recent turns
- Never cut mid-tool-call; the cut snaps **backwards to the start of the turn it
  lands in** (`entries.turn_id`, `plan_find_cut` in `context.c`) — not to "the
  previous user message", which would split a turn that opened with several
  inbox-drained user entries, and not forwards, which could drop the very turn
  being answered. Keeping a whole turn can overshoot the budget by one turn.
- Cutoff notice prepended when truncated
- Compaction: summarize old entries, reparent tree. Its keep boundary
  (`context_compaction_keep_from`) snaps the same way, so a summary never lands
  inside a turn; a branch that is one giant turn compacts to nothing.
- **Pin asymmetry**: a pinned entry (`entries.data $.pin=1`, hook `pin`) is
  re-included past the *read-time cut* — it is invisible for one request, not
  gone. Compaction's `keep_from` walk ignores pin entirely: a pinned entry old
  enough to be compacted is summarized like any other, and only the summary
  survives. Pin protects the cut, not the tree. Known and accepted (a pin that
  vetoed compaction would let one entry pin an unbounded branch); the durable
  way to carry something past compaction is the mechanical state coda below.
- **Mechanical state coda** (E2): the compaction entry is the model's prose
  plus a `--- carried state ---` section built parent-side *after* the
  compaction call returns (`compaction_state_coda`, `llm_proc.c`) — open
  approvals (pending and approved-but-unconsumed tickets), running sub-agent
  sessions, armed cron one-shots, queried live in one SQL pass. It is never
  sent to the compaction model, so the summary cannot drop or paraphrase it.
  Still exactly one role-4 entry; empty state appends nothing. A failed
  compaction writes no entry at all (no coda-only rows) — the failure counter
  in `compaction_record_outcome` records it. The read-time sliding window gets
  no coda: that state is already live in the `<RELEVANT_CONTEXT>` block and
  `check_session`.

## Letta Patterns NOT Adopted

| Letta Feature | CClaw Decision | Reason |
|---------------|---------------|--------|
| Shared blocks across agents | Implemented as explicit attachments (`agent_memory_blocks`); no global block | A NULL-agent block would silently apply to every future agent |
| Sleeptime background agent | Not implemented | Adds complexity; agent can manage own memory |
| Stateless mode | Not needed | Sessions are cheap, tree structure handles branching |
| PostgreSQL + pgvector | SQLite single-file | No server dependency, trivial deployment |
| 100K char block limit | 5K default | Smaller context windows, cheaper models |
