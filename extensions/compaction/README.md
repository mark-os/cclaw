# compaction

A `ContextThreshold` and `CacheExpiring` hook, on for every agent. When a
session grows past its trim threshold (`agent_models.trim_threshold`, half
the model's window unless set), the oldest part of it is replaced by a summary the agent's own model
writes, down to the prune target (`prune_target`, half the threshold unless
set):

- whole earlier turns first, oldest first;
- then, if that is not enough, the oldest steps of the turn still running —
  between two steps, so a long task can go on for as long as it needs. The
  user messages that opened the turn and its newest step always stay.

Two things are the script's, not the model's:

- **The actions already taken** — every call the runtime marks `protected`
  (a file written, a message sent, a job scheduled) — are listed under the
  summary from the calls themselves:

  ```
  Actions already taken in the part of the conversation this replaces (they happened; do not repeat one unless asked to):
  - file_write {"path":"src/sched.c","content":"…"} → wrote 2048 bytes to src/sched.c
  ```

  Without that, an agent that no longer remembers doing something tends to
  do it again.
- **One summary, not a stack.** An earlier summary just before the part being
  summarised is folded into the new one: its prose goes to the model, its
  list of actions is carried over line for line.

## When the cache is about to lapse

The same script is also a `CacheExpiring` hook. A session that has sat idle
for 90% of its model's cache lifetime (`agent_models.cache_lifetime`; on
Anthropic 3600 seconds unless set to 300) while above its prune target is
about to lose its prompt cache, so the next message will pay full price for
the whole session anyway: the cheapest moment to summarise it. Then:

- a sub-agent's session (`args.context.depth` above 0) is left alone — it is
  short-lived, and its parent reads its result, not its history;
- a top-level session is summarised down to **half** the prune target, the
  same way as above (oldest turns first; on an idle session every step of
  the last turn may go, never its answer). Each run removes at least half of
  what is there, so it cannot fire again until the session has doubled.

If nothing it may take would bring the session under its prune target, it
returns no edits. On this event nothing follows the script: no edits, a
failed run or no script at all leave the session exactly as it was — the
runtime never trims an idle session. An entry with no `cache_lifetime` never
fires it.

## When it fails to summarise

If the model cannot be reached, the script throws: a failed run. If nothing it
may take would bring the session under the threshold, it returns no edits
without asking the model. Either way, on `ContextThreshold`, the runtime's
own trim runs next and cuts the session to its target without a summary
(oldest turns, then oldest steps, and a one-line note). Nothing is deleted: the originals stay in the database.

## How it ships

The bundle is compiled into the binary (`SHIPPED_BUNDLES` in the Makefile)
and installed at every start into `~/.cclaw/extensions/compaction/`, with no
owner and published — the files there are a copy of the binary's, rewritten
on each start. The first install (a fresh database, or the first start of a
build that has it) attaches it to every agent; agents created later get it
from the config key `agent_default_extensions`.

## Switch it off, or replace it

For one agent — the agent is then on the runtime's trim alone:

```sql
-- cclaw sqlite3 --write ~/.cclaw/cclaw.db "…"
UPDATE agent_extensions SET enabled=0
 WHERE agent_name='<Agent>' AND extension_name='compaction';
```

For every agent: `UPDATE extensions SET enabled=0 WHERE name='compaction';`.
Both survive restarts and upgrades.

To change how it works, copy the bundle under another name, edit it, promote
it (an approval, like any extension) and attach it, then switch this one off
for the agents that use yours:

```sh
cp -r extensions/compaction ~/.cclaw/agents/<Agent>/workspace/extensions/my-compaction
# set "name": "my-compaction" in its extension.json
```

Editing the installed copy in `~/.cclaw/extensions/compaction/` does not last:
the next start writes the binary's version back.

## Settings

The constants at the top of `compact.qjs`:

| Constant | Default | |
|---|---|---|
| `PURPOSE` | `"chat"` | Which routing list writes the summary. The agent's own chat model, because it must read up to half a window. `"utility"` is cheaper, and safe only when that model's window is as large. |
| `PROMPT_SHARE` | 0.5 | How much of the window the summariser is shown. Longer tool results are cut until the part being summarised fits. |
| `SUMMARY_SHARE`, `SUMMARY_MIN`, `SUMMARY_MAX` | 0.1, 800, 6000 | The summary's length: a tenth of the target, in tokens, within those bounds. |
| `LLM_TIMEOUT` | 240 | Seconds per model tried. The hook's deadline is 600 (`"timeout"` in `extension.json`). |
| `EFFECTS_MAX` | 60 | The newest this many actions are listed; older ones are counted. |

The hook is sent the whole session up to 2 MiB (`"max_bytes"`), which the
default script heap (`js_heap_mb` 8) holds. On a model with a window of a
million tokens a session can be larger than that: the oldest turns are then
not sent, and the runtime's trim reaches them.

## When it fails

Three failed runs in a row (the model unreachable, a crash, a reply the
runtime refuses, the deadline) turn the hook off — each of the two hooks
counts its own: the default agent is told once, and the agents are on the
runtime's trim (or, for `CacheExpiring`, idle sessions are left as they are)
until it is turned back on.
A restart does not turn it back on; a new version of the bundle does, as does
the operator:

```sql
UPDATE hooks SET disabled=0, fail_count=0 WHERE extension_name='compaction';
```

## See what it did

- **The log**, one line per run: `hook ContextThreshold ext=compaction
  outcome=applied` (or `nothing`, `failed`, `rejected`, with a reason; the
  same with `CacheExpiring`, after a `hook CacheExpiring due` line), and a
  `trim session=…` line when the runtime's trim ran after it.
- **The record**, on the session's newest entry after each run: what left,
  the summary written, sizes before and after, and the script's own
  `record` (which turns or steps, which model, how many actions listed):

  ```sql
  SELECT id, json(data -> '$.turn_edits') FROM entries
   WHERE data -> '$.turn_edits' IS NOT NULL ORDER BY id DESC LIMIT 5;
  ```

- **The summariser's calls**: `cclaw resp list` shows them labelled
  `hook:compaction`; `cclaw resp <id> req` shows what it was sent.
- **The explorer**: the session's page shows each edit and the entries it
  replaced.
