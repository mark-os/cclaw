---
name: cclaw-troubleshoot
description: CClaw runtime investigation. Triggers on "why did the agent fail", "blank response", "debug session", "what happened", "troubleshoot", "investigate turn", "check logs", "update broke it", "roll back". Generic technique for any cclaw deployment — DB forensics, log locations, update/rollback recovery. No machine specifics here; keep those in a private, gitignored companion skill.
---

# cclaw-troubleshoot

Start here when a turn fails, hangs, returns blank, or behaves unexpectedly —
or when an update went wrong.

## Core discipline: DB forensics, not agent self-report

**Never trust the agent's claim of what happened — verify in the DB.**
`agent_models`, `grants`, `approvals.state` are truth; "Gemini is now active"
or "I found the grant" are not.

## Reaching the database

Durable state has one home: `cclaw.db` (`$CCLAW_DB_PATH`, default
`$HOME/.cclaw/cclaw.db` of the user the daemon runs as; a
`cclaw install --system` runs it as user `cclaw`, so `/home/cclaw/.cclaw/`).

Read it **in place**, with the daemon running, using the vendored shell:

```bash
cclaw sqlite3 <db> "SELECT ...;"
```

- **Read-only by default** (`--write` for destructive access) and drops root
  to the DB file's owner, so it is safe to run under `sudo` or as the daemon
  user (`sudo -u cclaw …`) — whichever the box allows. A WAL reader never
  blocks the daemon. Copy the DB out only to *replicate* (run a daemon or a
  migration against it), never just to read.
- **Prefer the installed binary over a dev build** for reads: it matches the
  DB's schema version exactly, and it decodes JSONB (a system `sqlite3` older
  than 3.45 dumps `llm_responses.body` as binary garbage — not corruption).
- SQL with quotes: pipe it over stdin (`printf '%s\n' "SELECT …;" | cclaw
  sqlite3 <db>`) rather than fighting shell quoting.
- A read-only connection cannot run WAL recovery; if the daemon is down after
  a crash and the open errors, copy `<db>` and `<db>-wal` aside together and
  query the copy.

## Where to look

1. **`cclaw --doctor`** first — one redacted report: version, DB
   open/schema/size, config load, provider reachability, sessions in
   non-idle states, the update/rollback situation (`[update]`), syslog tail.
   Cheap, and it answers "is anything obviously wrong" before you dig.

2. **Session entries** — the turn's DB trail:
   ```sql
   SELECT id, role, stop_reason, usage_in, usage_out, substr(content,1,80)
   FROM entries WHERE session_id=N ORDER BY id;
   ```
   Roles: 0=system 1=user 2=assistant 3=tool 4=compaction.
   `(empty placeholder)` in a role=2 row is the gateway's stand-in for a
   pure-tool-call turn (no text content) — not a bug by itself.

3. **Session state** — is it stuck?
   ```sql
   SELECT id, state, owner_instance, leaf_id FROM sessions WHERE id=N;
   ```
   States: `idle`, `llm_running`, `tool_running`, `compacting`,
   `rate_limited`, `awaiting_agent`, `awaiting_approval`. Any non-idle state
   has `owner_instance` set; non-idle with a **dead** owner (no live
   `processes` row) means the turn was orphaned — restart recovery
   (`db_recover_stale_sessions`) reclaims it at the next daemon start.
   `rate_limited` recovers on its own once the rolling-window usage drops
   below the cap — compute the per-minute usage histogram from `entries` to
   predict *when*, don't guess.

4. **Archived LLM responses** — always-on. Error entries cite `[resp #N]`;
   read that row with the binary, not raw SQL:
   ```bash
   cclaw resp            # most recent failure (what "[resp #N]" cites)
   cclaw resp <id>       # one row, pretty-printed
   cclaw resp <id> req   # the request body we sent (archived on failures)
   cclaw resp list 20    # recent rows, newest first
   ```
   Metadata queries are fine with any sqlite3:
   ```sql
   SELECT id, turn_id, model, status, length(body),
          request_body IS NOT NULL AS has_req,
          datetime(created_at,'unixepoch')
   FROM llm_responses WHERE session_id=N ORDER BY id DESC LIMIT 5;
   ```
   Statuses: `ok` | `empty` | `malformed` | `http_<code>` | `timeout` |
   `network_error`. Retention is per-class (`llm_response_archive_max` keeps
   N `ok` rows plus N failure rows), so a cited failure outlives routine
   traffic even after many turns.

5. **Other tables worth checking directly:**
   - `channel_outbox` — `status`, `attempts`,
     `json_extract(payload,'$.text')`. Confirms delivery vs. silent drop; a
     queued-but-undelivered row outlives the turn.
   - `inbox` — `consumed` + `created_at` settles "did the bot even see
     message X" disputes (mention-gate drops never reach `entries`).
   - `tool_calls` — `status` (not `state`) per `call_id`; pending rows past
     a turn's end are the parking/dispatch bug, not model confusion.
   - `approvals` / `grants` — compare `created_at` timestamps to check
     whether a "redundant" request actually wrote a new row (PK
     `(agent_name, kind, value)` on grants makes true duplicates impossible;
     the real question is usually "did a wasted approval-park happen").
   - `memory_blocks` (metadata) + `memory_entries` (text, joined on
     `agent_name`+`block_label`, ordered by `pos`). `memory_blocks.placement`
     decides where a block renders: `system` → system prompt (cached prefix),
     `context` → per-turn session context block.

6. **Reproduction (CLI)** — default is error-only; opt in for detail:
   ```bash
   cclaw -v --new -p "test" 2>debug.txt   # debug: timing, SQL profiling, retry decisions
   cclaw -vv --new -p "test" 2>debug.txt  # trace: + full req/resp JSON
   ```
   `-p` with piped/non-tty stdin auto-selects the most recent session;
   `-s <id>` pins one — useful for scripted multi-turn repro. Approvals park
   and expire between `-p` runs (no interactive approver); `--auto-approve`
   answers whatever is parked, blindly — inspect `approvals` before and after.

7. **Daemon logs** — depends on the init system the install chose:
   ```bash
   journalctl -u cclaw -f                 # systemd, system unit (cclaw install --system)
   journalctl --user -u cclaw -f          # systemd, user unit (cclaw install)
   journalctl -t cclaw --since "5 min ago" # syslog identifier only — misses raw stdout/stderr
   tail -f /var/log/cclaw.log             # SysV init targets (no journald)
   ```
   Prefer `-u` on a systemd box: `-t cclaw` only catches explicit syslog
   calls; the unit sets `StandardOutput=journal`, so `-u` also has the
   crash/abort output.

8. **Process hangs** — always redirect to a file, never pipe a live cclaw
   through head/grep/tail (SIGPIPE kills it mid-run). Before killing
   orphans, verify PIDs with `ps` — a box may run more than one daemon
   (a production one as its own user beside a dev one); never kill by
   inference. `make debug` (ASan/UBSan) for crash stack traces.

## Update went wrong

`cclaw update` leaves a trail; read it before touching anything:

```bash
cclaw --doctor          # [update] section: last outcome, guard, leftovers
```

- `update.last` (shown there) is `{tag, at, outcome}`. `ok`/`verified` means
  the daemon came back and ran 5 min; `reverted_no_restart` means the new
  build never registered and the old binary is already back;
  `crash_loop_reverted` means the guard put `.prev` back after three starts;
  `crash_loop_no_revert` means it *could not* (the new build migrated the DB
  and `.prev` cannot open it) — that is the case for `cclaw rollback`.
- **`cclaw rollback`** restores `<db>.preupdate` and `<self>.prev` together.
  It prints the plan (snapshot age, entries that will be discarded, which
  build returns) and asks; the swap happens only while nothing has the DB
  open — via the daemon's restart, or at once when none runs. Nothing is
  deleted: the replaced DB is kept as `<db>.failed-<tag>`, the replaced
  binary as `<self>.bad`.
- Leftover files beside the DB/binary (`.preupdate`, `.prev`, `.bad`,
  `.v<N>.bak`, `.failed-*`, a pending `.rollback`) are each explained by
  `--doctor`; delete only what it calls safe to delete.
- If the daemon runs as another user, run these verbs as that user or under
  `sudo` — `rollback` needs write access to both the DB directory and the
  binary's directory.

Reference: `specs/operations.md` (backup, restore, update, rollback).

## Model/prompt drift — check before assuming "weak model"

- **Which model actually ran a turn** is `agent_models` (the routing list),
  cross-checked against `entries` grouped by `model`. A comprehension miss
  can look like a weak-model problem when it's actually a strong model
  over-weighting a system-prompt instruction (e.g. "silence is the default").
- **`agents.system_prompt` is a frozen copy taken at agent creation** — it
  does NOT track `templates/default_system_prompt.md` improvements
  automatically. Diff the stored prompt against the current template before
  trusting that "current guidance" is even present.
- `channel_routes.system_prompt_suffix` is composed live at request-build
  time (`llm_payload.c`) — edits take effect on the *next* turn, no restart.
  Channel *extension* code is loaded once at runner startup — after editing
  it, sync the live copy (`~/.cclaw/extensions/<name>/`) **and** restart the
  channel (`cclaw channel restart <name>`); editing the template alone does
  nothing.

## Boundary-translation pattern

Anywhere the model would need to reproduce an opaque ID (chat-platform
snowflakes, emoji IDs, secrets), prefer a **seen-directory learned from live
traffic** (`channel.getState/setState`) over exposing the ID directly. The
model works in human-readable names; the extension substitutes real IDs only
for known, observed entries. Unknown/hallucinated names silently fail to
resolve rather than risk targeting the wrong entity — same shape as
`{{SECRET:name}}` interpolation.

## Reference docs

- `specs/error-handling.md` — failure taxonomy (E1–E14), retry/backoff/fallback
  policy, response resolution, log levels, model degradation on repeated
  failure
- `specs/operations.md` — backup, restore, update, rollback
- `specs/providers.md` — provider fallback chain
- `specs/schema.md` — table definitions for entries, sessions, llm_responses
