# Daemon/Agent Separation Refactor

## Problem Statement

CClaw currently uses a single shared SQLite DB for all state (daemon coordination, agent sessions, inbox, config). Agents write directly to shared tables (spawn_queue, approvals, kv) creating tight coupling. Config lives in JSON files on disk. This refactor cleanly separates concerns following Unix principles: daemon as init/service-broker, agents as isolated users with home directories, processes as cheap/disposable, communication via exit codes + DB reads.

## Requirements

1. Three DB files: `daemon.db` (registry, permissions, cron, spawn_queue, channels, provider config), `journal.db` (all logs from daemon + agents), per-agent `agents/<name>/agent.db` (sessions, entries, inbox, js_tools, memory_blocks)
2. No JSON config files — all config in SQLite (daemon.db for permissions/policy, agent env vars for runtime)
3. Strict process separation — agents write only to their own DB, communicate intent via exit codes
4. Daemon reads agent DB after reap to discover requests (spawn, approval, config change)
5. Daemon writes to agent DBs only for inbox delivery
6. Log collector process — receives all stdout/stderr via pipes, writes to journal.db
7. Config injected to agents via `CCLAW_*` env vars at fork time
8. Agents can optionally have read-only access to daemon.db (landlock-granted)
9. CLI remains standalone (opens agent DB directly, no daemon needed)
10. Landlock policy stored in daemon.db, applied by daemon at fork

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        DAEMON                                │
│  daemon.db (registry, permissions, cron, spawn_queue,        │
│             channel_bindings, provider config)                │
│  Threads: Telegram poller, civetweb, cron scheduler,         │
│           heartbeat                                          │
│  Epoll: signal pipe + SIGCHLD                                │
│  Forks: agent processes (exit code signaling)                │
│  Spawns: log collector (once, at startup)                    │
└──────┬──────────────────────────────────┬────────────────────┘
       │ fork+exec                        │ pipe stdout/stderr
       ▼                                  ▼
┌──────────────┐                 ┌─────────────────┐
│ Agent proc   │                 │  Log Collector   │
│ agent.db     │                 │  journal.db      │
│ (own DB)     │                 │  epoll on pipes  │
│ landlock     │                 │  writes all logs │
│ exit code    │                 └─────────────────┘
│ signals      │
│ intent       │
└──────────────┘

┌──────────────┐
│     CLI      │
│ standalone   │
│ opens agent  │
│ DB directly  │
│ no daemon    │
└──────────────┘
```

## Exit Code Protocol

| Code | Meaning | Daemon Action |
|------|---------|---------------|
| 0 | Turn complete, deliver response | Read last assistant entry from agent DB, deliver to channel |
| 1 | Turn complete with error | Log error, mark session idle |
| 2 | Spawn sub-agent requested | Read last tool_call from agent DB, fork child |
| 3 | Approval requested | Read last tool_call from agent DB, notify admin |
| 4 | Config change requested | Read last tool_call from agent DB, validate + apply |
| 127 | exec failed | Log error |
| 128+N | Killed by signal N | Log crash, synthesize error |

## DB Schema Split

### daemon.db

```sql
CREATE TABLE agents (
  name TEXT PRIMARY KEY,
  status TEXT NOT NULL DEFAULT 'active',
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  updated_at INTEGER NOT NULL DEFAULT (unixepoch())
);

CREATE TABLE agent_config (
  agent_name TEXT NOT NULL,
  key TEXT NOT NULL,
  value TEXT NOT NULL,
  PRIMARY KEY (agent_name, key)
);
-- Keys: model, workspace, tools (JSON array), allowed_hosts (JSON array),
-- read_access (JSON array), max_iterations, shell_timeout,
-- landlock_net_ports (JSON array), daemon_db_read (0|1)

CREATE TABLE providers (
  name TEXT PRIMARY KEY,
  base_url TEXT NOT NULL,
  model TEXT NOT NULL,
  context_window INTEGER NOT NULL DEFAULT 128000
);

CREATE TABLE kv (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
-- Global config + encrypted secrets (enc: prefix)

CREATE TABLE channel_bindings (
  channel_type TEXT NOT NULL,
  channel_id TEXT NOT NULL,
  agent_name TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  PRIMARY KEY (channel_type, channel_id)
);

CREATE TABLE tg_chat_sessions (
  chat_id INTEGER PRIMARY KEY,
  session_id INTEGER NOT NULL,
  agent_name TEXT NOT NULL
);

CREATE TABLE spawn_queue (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  parent_agent TEXT NOT NULL,
  parent_session_id INTEGER NOT NULL,
  tool_call_id TEXT,
  child_agent TEXT,
  child_session_id INTEGER,
  task TEXT NOT NULL,
  background INTEGER NOT NULL DEFAULT 0,
  depth INTEGER NOT NULL DEFAULT 1,
  status TEXT NOT NULL DEFAULT 'pending',
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE INDEX idx_spawn_pending ON spawn_queue(status) WHERE status='pending';

CREATE TABLE cron_jobs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  agent_name TEXT NOT NULL,
  name TEXT NOT NULL,
  cron_expr TEXT NOT NULL,
  session_id INTEGER NOT NULL,
  task TEXT NOT NULL,
  enabled INTEGER NOT NULL DEFAULT 1,
  next_run_at INTEGER NOT NULL DEFAULT 0,
  last_run_at INTEGER,
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);

CREATE TABLE approvals (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  agent_name TEXT NOT NULL,
  session_id INTEGER NOT NULL,
  type TEXT NOT NULL,
  payload TEXT NOT NULL,
  status TEXT NOT NULL DEFAULT 'pending',
  admin_chat_id INTEGER,
  notified INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  resolved_at INTEGER
);
CREATE INDEX idx_approvals_pending ON approvals(status) WHERE status='pending';
```

### agents/\<name\>/agent.db

```sql
CREATE TABLE sessions (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT,
  leaf_id INTEGER DEFAULT -1,
  parent_session_id INTEGER DEFAULT -1,
  depth INTEGER NOT NULL DEFAULT 0,
  state TEXT NOT NULL DEFAULT 'idle',
  last_route TEXT,
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  updated_at INTEGER NOT NULL DEFAULT (unixepoch())
);

CREATE TABLE entries (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL,
  parent_id INTEGER NOT NULL DEFAULT -1,
  original_parent_id INTEGER,
  turn_id INTEGER,
  role INTEGER NOT NULL DEFAULT 1,
  content TEXT,
  tool_calls TEXT,
  tool_call_id TEXT,
  tool_name TEXT,
  is_error INTEGER NOT NULL DEFAULT 0,
  stop_reason INTEGER NOT NULL DEFAULT 0,
  model TEXT,
  usage_in INTEGER,
  usage_out INTEGER,
  token_estimate INTEGER,
  content_bytes INTEGER,
  tool_call_count INTEGER NOT NULL DEFAULT 0,
  data TEXT,
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE INDEX idx_entries_session ON entries(session_id, id);
CREATE INDEX idx_entries_parent ON entries(parent_id);
CREATE INDEX idx_entries_session_role ON entries(session_id, role);
CREATE INDEX idx_entries_turn ON entries(session_id, turn_id);
CREATE INDEX idx_entries_stop_reason ON entries(session_id, stop_reason) WHERE stop_reason != 0;
CREATE INDEX idx_entries_plan ON entries(parent_id, session_id, id, role, stop_reason, token_estimate, tool_call_count);

CREATE VIRTUAL TABLE entries_fts USING fts5(
  content, content=entries, content_rowid=id
);
CREATE TRIGGER entries_ai AFTER INSERT ON entries BEGIN
  INSERT INTO entries_fts(rowid, content) VALUES (
    new.id,
    CASE new.role
      WHEN 1 THEN new.content
      WHEN 0 THEN new.content
      WHEN 3 THEN COALESCE(new.tool_name,'') || ' ' || COALESCE(new.content,'')
      WHEN 2 THEN COALESCE(new.content,'') || ' ' || COALESCE(new.tool_calls,'')
      ELSE COALESCE(new.content,'')
    END
  );
END;

CREATE TABLE inbox (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL,
  source TEXT NOT NULL,
  payload TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  consumed INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX idx_inbox_pending ON inbox(session_id, consumed) WHERE consumed = 0;

CREATE TABLE js_tools (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL,
  name TEXT NOT NULL,
  description TEXT,
  parameters_json TEXT,
  code TEXT NOT NULL,
  UNIQUE(session_id, name)
);

CREATE TABLE memory_blocks (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  label TEXT NOT NULL UNIQUE,
  value TEXT NOT NULL DEFAULT '',
  description TEXT,
  char_limit INTEGER NOT NULL DEFAULT 5000,
  read_only INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  updated_at INTEGER NOT NULL DEFAULT (unixepoch())
);

CREATE TABLE kv (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
-- Agent-local preferences only (no secrets)
```

### journal.db

```sql
CREATE TABLE log (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  source TEXT NOT NULL,
  pid INTEGER,
  session_id INTEGER,
  stream INTEGER DEFAULT 1,
  line TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE INDEX idx_log_source ON log(source, created_at);
CREATE INDEX idx_log_time ON log(created_at);
```

## Log Collector Design

- Daemon spawns one long-lived log collector child at startup
- Communication via unix domain socketpair (daemon keeps one end, collector the other)
- On each agent fork: daemon creates pipe, dups write-end to child stdout/stderr, sends read-end to collector via `SCM_RIGHTS` with metadata (agent_name, session_id, pid)
- Collector epoll's on all received fds, reads lines, batches inserts to journal.db (flush every 100ms or 64 lines)
- Daemon's own stdout/stderr also piped to collector (source="daemon")
- Collector crash doesn't kill agents (pipe write gets EPIPE, agent ignores)
- Collector is unsandboxed (needs journal.db write access)

## Agent Config Introspection

Agents learn their config via environment variables injected by daemon at fork:

- `CCLAW_AGENT_NAME` — agent identity
- `CCLAW_AGENT_DB` — path to own agent.db
- `CCLAW_WORKSPACE` — writable workspace path
- `CCLAW_MODEL` — LLM model name
- `CCLAW_MAX_ITERATIONS` — max tool loop iterations
- `CCLAW_ALLOWED_HOSTS` — comma-separated hostnames
- `CCLAW_TOOLS` — comma-separated tool whitelist
- `CCLAW_SHELL_TIMEOUT` — seconds
- `CCLAW_INJECTED_API_KEY` — decrypted provider API key
- `CCLAW_DAEMON_DB` — path to daemon.db (if read access granted)

Agent can introspect by reading its own `CCLAW_*` env vars.

## Task Breakdown

### Task 1: Define and implement the three DB schemas

**Objective:** Create separate schema files and initialization code for daemon.db, agent.db, and journal.db.

**Implementation:**
- Create `templates/daemon_schema.sql`, `templates/agent_schema.sql`, `templates/journal_schema.sql`
- Add `db_open_daemon()`, `db_open_agent()`, `db_open_journal()` functions
- All three use WAL mode + busy_timeout
- Migrate existing `schema.sql` content into appropriate new files

**Tests:** Unit test opens each DB type, verifies tables exist, verifies WAL mode.

**Demo:** `make test_db_split` passes — three DB files created independently with correct schemas.

---

### Task 2: Implement agent_config table and eliminate agent.json

**Objective:** Replace `agent.json` files with `agent_config` key-value table in daemon.db.

**Implementation:**
- `agent_config` table: `(agent_name TEXT, key TEXT, value TEXT, PRIMARY KEY(agent_name, key))`
- New function: `agent_config_load_from_db(sqlite3 *daemon_db, const char *agent_name)` → `AgentConfig*`
- Migration: on startup, if `agents/<name>/agent.json` exists and no DB rows, import and delete
- Remove JSON-based `agent_config_load()` after migration

**Tests:** Config round-trips through daemon.db; migration from agent.json works.

**Demo:** Agent config lives in daemon.db; agent.json files imported on first run and no longer needed.

---

### Task 3: Implement exit code protocol in agent process

**Objective:** Replace agent's direct writes to spawn_queue/approvals with exit codes.

**Implementation:**
- Define `agent_exit.h`: `AGENT_EXIT_OK=0`, `AGENT_EXIT_ERROR=1`, `AGENT_EXIT_SPAWN=2`, `AGENT_EXIT_APPROVAL=3`, `AGENT_EXIT_CONFIG=4`
- `tool_launch_agent_handler()`: return sentinel result, agent loop exits with code 2
- `tool_approval_handler()`: return sentinel, agent exits with code 3
- `agent_run()` detects sentinels, propagates exit code
- `run_agent_turn()` calls `_exit()` with the code

**Tests:** Agent exits with code 2 after spawn_agent, code 3 after approval_request.

**Demo:** Agent process exits with correct code; daemon can read triggering tool_call from agent DB.

---

### Task 4: Modify daemon reap logic to handle exit codes

**Objective:** Update `reap_children()` to dispatch based on exit code.

**Implementation:**
- Exit 0: read last assistant entry from agent DB, deliver to channel
- Exit 2: read last tool_call, extract spawn_agent args, insert into daemon.db spawn_queue, process
- Exit 3: read last tool_call, extract approval args, insert into daemon.db approvals, notify admin
- Exit 4: read last tool_call, validate config change, apply
- Exit 1/signal: log error, mark session idle
- Daemon opens agent DB read-only for these reads (separate handle, opened after reap, closed immediately)

**Tests:** Fork mock agent exiting with code 2 → daemon creates spawn_queue entry. Same for code 3.

**Demo:** Full cycle — agent calls spawn_agent → exits 2 → daemon forks sub-agent → result delivered.

---

### Task 5: Split DB access — agent opens only its own DB

**Objective:** Agent process opens only `agents/<name>/agent.db`. All config from env vars.

**Implementation:**
- Daemon sets env vars before exec: `CCLAW_AGENT_NAME`, `CCLAW_AGENT_DB`, `CCLAW_WORKSPACE`, `CCLAW_MODEL`, etc.
- `run_agent_turn()` reads `CCLAW_AGENT_DB`, opens that DB
- Remove `config_load_from_kv()` from agent — config from env vars
- Agent's `db_query` tool operates on agent.db only
- Memory blocks, js_tools in agent.db
- Remove `CCLAW_DB_PATH` env var usage from agent

**Tests:** Agent completes full turn with only agent.db access. Agent cannot write daemon.db.

**Demo:** Agent runs full turn using only its own DB; no shared DB access.

---

### Task 6: Daemon writes inbox to agent DB

**Objective:** All inbox inserters write to target agent's DB file.

**Implementation:**
- `daemon_inbox_insert(agent_name, session_id, source, payload)` — opens agent's DB, inserts, closes
- Optional: LRU cache of open agent DB handles (max 16)
- Telegram thread: resolve agent from channel_binding, write to agent DB
- Cron/heartbeat: same pattern
- Sub-agent completion: write tool_result to parent's inbox in parent's DB

**Tests:** inbox_insert to agent DB from daemon. Telegram message arrives in correct agent inbox.

**Demo:** Telegram message → agent inbox → daemon forks → agent responds.

---

### Task 7: Implement log collector process

**Objective:** Long-lived child process receiving stdout/stderr from all agents via pipes, writing to journal.db.

**Implementation:**
- New `src/log_collector.c`
- Unix domain socketpair for fd passing
- On agent fork: create pipe, dup to child stdout/stderr, send read-end via `SCM_RIGHTS`
- Collector: epoll on fds, read lines, batch insert to journal.db
- Daemon stdout/stderr also piped to collector
- Flush every 100ms or 64 lines

**Tests:** Collector receives lines from forked child, writes to journal.db. Handles multiple concurrent writers.

**Demo:** Start daemon, trigger agent turn, query journal.db — see interleaved logs with source attribution.

---

### Task 8: Migrate session state management to agent DB

**Objective:** Session state lives in agent.db. Daemon reads from agent DB before forking.

**Implementation:**
- Agent sets own state: "running" on startup, "idle" on exit 0/1, "waiting" on exit 2
- Daemon reads state from agent DB before fork decision
- Startup recovery: scan all agent DBs for "running" state, reset to "idle"
- Daemon tracks `{pid → agent_name, session_id}` in memory

**Tests:** State transitions in agent DB. Daemon startup recovery scans correctly.

**Demo:** Agent lifecycle visible in agent.db; daemon refuses fork when state != idle.

---

### Task 9: Migrate spawn_queue to daemon.db with exit-code flow

**Objective:** spawn_queue in daemon.db, populated by daemon after reaping exit-code-2 agents.

**Implementation:**
- After reap with exit 2: open agent DB, read last tool_calls, parse spawn_agent args
- Insert into daemon.db spawn_queue, process
- Blocking: agent sets own state to "waiting" before exit
- Sub-agent completion: daemon reads child result, writes to parent inbox, signals parent

**Tests:** Full blocking sub-agent lifecycle end-to-end.

**Demo:** Parent spawns child → exits 2 → daemon processes → child completes → parent resumes.

---

### Task 10: Migrate approvals to daemon.db with exit-code flow

**Objective:** Approvals in daemon.db, populated after reaping exit-code-3 agents.

**Implementation:**
- After reap with exit 3: read tool_call, insert into daemon.db approvals
- Agent state "waiting" (set by agent before exit)
- Admin resolves → daemon writes result to agent inbox → sets state "idle" → signals

**Tests:** Full approval flow end-to-end.

**Demo:** Agent requests whitelist → exits → admin approves → agent resumes.

---

### Task 11: Migrate cron to daemon.db

**Objective:** Cron jobs in daemon.db, daemon owns scheduler.

**Implementation:**
- cron_jobs table with `agent_name` column in daemon.db
- Scheduler reads from daemon.db
- On fire: daemon writes task to agent's inbox (in agent's DB)
- Cron management: admin-only for v1 (Telegram commands, bootstrap)
- Agents with daemon.db read access can query their own cron schedule

**Tests:** Cron fires, delivers to correct agent inbox.

**Demo:** Cron fires → message in agent inbox → agent wakes and processes.

---

### Task 12: Refactor CLI as standalone agent runner

**Objective:** CLI opens agent DB directly, no daemon needed. Extract shared setup code.

**Implementation:**
- Extract common agent setup into `agent_setup()` function
- CLI opens `agents/<name>/agent.db` directly
- CLI loads config from env vars or defaults
- CLI lacks: spawn_agent, cron, approval_request
- CLI keeps: shell, file, js, web_fetch, db_query, memory
- Remove daemon-detection logic from CLI

**Tests:** CLI runs full turn on agent DB without daemon.

**Demo:** `./build/cclaw --agent-name=mark` runs interactive REPL with local tools.

---

### Task 13: Update landlock to use daemon.db config

**Objective:** Landlock policy from daemon.db, applied via env vars in child.

**Implementation:**
- Daemon reads agent_config: workspace, read_access, net_ports
- Passes via env: `CCLAW_WORKSPACE`, `CCLAW_READ_ACCESS` (colon-separated), `CCLAW_NET_PORTS`
- Agent reads env, calls `landlock_apply()` with parsed values
- If daemon_db_read=1, include daemon.db path in landlock read rules
- Agent DB always RW in landlock

**Tests:** Landlock applied from env vars. Agent cannot write outside workspace.

**Demo:** Sandbox per daemon.db config; config change affects next fork.

---

### Task 14: Bootstrap and agent creation flow

**Objective:** Bootstrap works with new DB split.

**Implementation:**
- `daemon_bootstrap()`: creates ephemeral agent dir + agent.db, registers in daemon.db
- `create_agent` tool: exit code 4, daemon creates new agent (dir + agent.db + daemon.db rows)
- `configure_provider`: exit code 4, daemon writes encrypted key to daemon.db
- `configure_channel`: exit code 4, daemon writes binding to daemon.db
- All bootstrap tools use exit code 4 — daemon dispatches by tool name

**Tests:** Full bootstrap: ephemeral configures provider → creates named agent → agent works.

**Demo:** Fresh install → bootstrap → named agent responds to Telegram.

---

### Task 15: Integration testing and migration

**Objective:** End-to-end tests + migration script for existing installations.

**Implementation:**
- Migration script: reads old cclaw.db, creates daemon.db + per-agent agent.db, copies rows
- Integration tests: full lifecycle, restart recovery, CLI standalone, log collector
- Remove old `schema.sql`, update `gen_templates.sh`

**Tests:** All existing tests updated. New integration tests for cross-DB flows.

**Demo:** Existing installation migrates cleanly; all features work.
