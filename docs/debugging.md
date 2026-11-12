# Debugging CClaw

Handy commands for diagnosing a misbehaving agent. The real DB is
`~/.cclaw/cclaw.db` (override with `CCLAW_DB_PATH`); its `-wal`/`-shm` siblings
hold state not yet written back to the main file, so query the live DB rather
than a copy of just the `.db` file.

```bash
DB=~/.cclaw/cclaw.db   # used below
```

The commands below use `cclaw sqlite3`: the standard SQLite shell, built into
the binary from the same SQLite the daemon uses. Prefer it to the system
`sqlite3`, which may be missing on a small box, and which renders the JSONB
columns (`llm_responses.body`, `request_body`) as binary garbage if it is older
than 3.45. That looks like corruption but is just the storage format.

It also has two safety properties the plain shell lacks: it opens the DB
**read-only by default** (pass `--write` for changes), and when run as root
against another user's DB it first switches to the DB file's owner, so it never
leaves root-owned `-wal`/`-shm` files that lock the daemon out.

## Diagnosing a failed LLM turn

A failed turn shows up in chat as `error: LLM request failed [resp #N]` (or
`error: rate limited`, `error: provider server error`, …). The `[resp #N]` is the
`llm_responses.id` that holds the archived request and reply. **Every** failed
attempt is archived — non-2xx, timeouts, empty bodies, and failures on our side
— with the payload we sent in `request_body`.

The quickest way in is `cclaw resp`:

```bash
cclaw resp              # the most recent failure
cclaw resp 1234         # one row (the N in "[resp #N]")
cclaw resp 1234 req     # the request we sent for that row
cclaw resp list 20      # the last 20 rows
```

`status` values: `ok | empty | malformed | ingest_error | http_<code> | timeout |
network_error`. `ingest_error` means the provider replied fine but we failed to
store the reply (usually `SQLITE_BUSY` under daemon+CLI contention) — it is
*not* the model's fault, and re-running the turn usually works.

The same data in SQL:

```bash
# The most recent failure: what we sent and what came back.
cclaw sqlite3 "$DB" "SELECT id, session_id, iteration_id, model, status,
  datetime(created_at,'unixepoch') AS at,
  CAST(request_body AS TEXT) AS request,
  CAST(body AS TEXT)         AS response
  FROM llm_responses WHERE status != 'ok'
  ORDER BY id DESC LIMIT 1;"

# Failure breakdown — is it timeouts, rate limits, or ingest_error?
cclaw sqlite3 "$DB" "SELECT status, count(*) FROM llm_responses
  GROUP BY status ORDER BY 2 DESC;"

# Every archived attempt for one LLM request (retries + fallback models share an iteration_id).
cclaw sqlite3 "$DB" "SELECT id, model, status, length(CAST(body AS TEXT)) AS blen
  FROM llm_responses WHERE session_id = 42 AND iteration_id = 7 ORDER BY id;"
```

The archive keeps the newest `llm_response_archive_max` rows (default 500;
`0` disables archiving, a negative value keeps everything):

```bash
cclaw sqlite3 --write "$DB" "UPDATE config SET value='-1'
  WHERE key='llm_response_archive_max';"   # keep all rows while debugging
```

## Live request/response on stderr

```bash
./build/cclaw --trace -p "..."        # full LLM req + resp JSON to stderr (-vv)
./build/cclaw --debug -p "..."        # timing, context stats (-v)
```

## Inspecting a session

```bash
# Recent sessions.
cclaw sqlite3 "$DB" "SELECT id, agent_name, state, datetime(updated_at,'unixepoch')
  FROM sessions ORDER BY updated_at DESC LIMIT 10;"

# The conversation (entries) for a session.
cclaw sqlite3 "$DB" "SELECT id, turn_id, iteration_id, type, substr(content,1,80)
  FROM entries WHERE session_id = 42 ORDER BY id;"

# Tool calls + their status for a session.
cclaw sqlite3 "$DB" "SELECT call_id, name, status, result_entry_id
  FROM tool_calls WHERE session_id = 42 ORDER BY entry_id;"

# A stuck session: anything not idle, and which process owns it.
cclaw sqlite3 "$DB" "SELECT id, state, owner_instance FROM sessions WHERE state != 'idle';"
```

A session that stays non-idle while its `owner_instance` no longer appears in
the `processes` table was left behind by a process that died; the daemon
reclaims such sessions on its own.

## Tools, grants, approvals

```bash
# What tools exist: built-in (extension_name NULL) or from an extension.
cclaw sqlite3 "$DB" "SELECT name, extension_name, agent_name, enabled FROM tools ORDER BY name;"

# An agent's sandbox profile and grants (tools, hosts, paths).
cclaw sqlite3 "$DB" "SELECT sandbox_profile FROM agents WHERE name = 'Assistant';"
cclaw sqlite3 "$DB" "SELECT kind, value FROM grants WHERE agent_name = 'Assistant';"

# Approvals still waiting for a decision.
cclaw sqlite3 "$DB" "SELECT id, tool_name, args_json, datetime(requested_at,'unixepoch')
  FROM approvals WHERE state = 'pending';"
```

## Starting clean

Schema changes ship as forward patches applied automatically at startup, so an
existing DB normally keeps working across upgrades. A DB older than the
build's floor, or newer than the build, is refused with a message telling you
to delete it. To start over, delete the DB and its WAL files — and only those;
leave backups and `.cclaw_key` alone:

```bash
rm -f ~/.cclaw/cclaw.db ~/.cclaw/cclaw.db-wal ~/.cclaw/cclaw.db-shm
# Also stale channel pipes/sockets if a run was killed:
rm -f ~/.cclaw/cclaw.*.pipe ~/.cclaw/cclaw.*.sock
```

For problems after an update, `cclaw rollback` restores the previous binary
and the pre-update DB snapshot together; see
[specs/operations.md](../specs/operations.md).
