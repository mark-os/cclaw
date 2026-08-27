# Debugging CClaw

Handy commands for diagnosing a misbehaving agent. The real DB is
`~/.cclaw/cclaw.db` (override with `CCLAW_DB_PATH`); its `-wal`/`-shm` siblings
hold uncheckpointed state, so query the live DB, don't copy just the `.db` file.

```bash
DB=~/.cclaw/cclaw.db   # used below
```

Every `sqlite3` below can be `cclaw sqlite3` instead — the same upstream shell,
built from the amalgamation the daemon links against. Prefer it on deployment
boxes: the system `sqlite3` may be missing entirely, and anything older than
3.45 renders the JSONB columns (`llm_responses.body`, `request_body`) as binary
garbage, which reads as corruption but is just the storage format.

## Diagnosing a failed LLM turn

A failed turn shows up in chat as `error: LLM request failed [resp #N]` (or
`error: rate limited`, `error: provider server error`, …). The `[resp #N]` is the
`llm_responses.id` that holds the archived request + reply. **Every** failed
attempt is archived — non-2xx, timeouts, empty bodies, and our-side ingest
failures — with the payload we sent in `request_body`.

`status` values: `ok | empty | malformed | ingest_error | http_<code> | timeout |
network_error`. `ingest_error` means the provider replied fine but we failed to
store it (usually `SQLITE_BUSY` under daemon+CLI contention) — the response is
*not* the model's fault; re-running the turn usually works.

```bash
# The most recent failure: what we sent and what came back.
sqlite3 "$DB" "SELECT id, session_id, iteration_id, model, status,
  datetime(created_at,'unixepoch') AS at,
  CAST(request_body AS TEXT) AS request,
  CAST(body AS TEXT)         AS response
  FROM llm_responses WHERE status != 'ok'
  ORDER BY id DESC LIMIT 1;"

# A specific cited row (the N in "[resp #N]").
sqlite3 "$DB" "SELECT CAST(request_body AS TEXT), CAST(body AS TEXT)
  FROM llm_responses WHERE id = 1234;"

# Failure breakdown — is it timeouts, rate limits, or ingest_error?
sqlite3 "$DB" "SELECT status, count(*) FROM llm_responses
  GROUP BY status ORDER BY 2 DESC;"

# Every archived attempt for one LLM request (retries + fallback models share an iteration_id).
sqlite3 "$DB" "SELECT id, model, status, length(CAST(body AS TEXT)) AS blen
  FROM llm_responses WHERE session_id = 42 AND iteration_id = 7 ORDER BY id;"
```

Archiving is ring-buffered by config `llm_response_archive_max` (default 500;
`0` disables, `<0` keeps everything):

```bash
sqlite3 "$DB" "INSERT OR REPLACE INTO config(key,value)
  VALUES('llm_response_archive_max','-1');"   # keep all rows while debugging
```

## Live request/response on stderr

```bash
./build/cclaw --trace -p "..."        # full LLM req + resp JSON to stderr
./build/cclaw --debug -p "..."        # timing, context stats, SQL profiling
```

## Inspecting a session

```bash
# Recent sessions.
sqlite3 "$DB" "SELECT id, agent_name, state, datetime(updated_at,'unixepoch')
  FROM sessions ORDER BY updated_at DESC LIMIT 10;"

# The conversation branch (entries) for a session.
sqlite3 "$DB" "SELECT id, turn_id, iteration_id, type, substr(content,1,80)
  FROM entries WHERE session_id = 42 ORDER BY id;"

# Tool calls + their status for a session.
sqlite3 "$DB" "SELECT call_id, name, status, result_entry_id
  FROM tool_calls WHERE session_id = 42 ORDER BY entry_id;"

# A stuck session: what state is it parked in?
sqlite3 "$DB" "SELECT id, state FROM sessions WHERE state NOT IN ('done','idle');"
```

## Tools, grants, approvals

```bash
# What tools exist and their policy/recipe metadata.
sqlite3 "$DB" "SELECT name, builtin, agent_name FROM tools ORDER BY name;"

# An agent's grants (hosts, tools, paths) and sandbox profile.
sqlite3 "$DB" "SELECT sandbox_profile FROM agents WHERE name = 'default';"
sqlite3 "$DB" "SELECT kind, value FROM grants WHERE agent_name = 'default';"

# Pending / unresolved approvals.
sqlite3 "$DB" "SELECT id, tool_call_id, status, datetime(created_at,'unixepoch')
  FROM approvals WHERE status = 'pending';"
```

## Starting clean

No migrations: a schema change means the old DB is incompatible. Delete the whole
family before running a freshly-changed binary.

```bash
rm -f ~/.cclaw/cclaw.db ~/.cclaw/cclaw.db-wal ~/.cclaw/cclaw.db-shm
# Also stale channel sockets/pipes if a run was killed:
rm -f ~/.cclaw/cclaw.db.*.pipe ~/.cclaw/cclaw.db.*.sock
```
