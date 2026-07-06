-- cclaw.db unified schema
-- Single file, WAL mode. CLI and daemon are peers sharing one source of truth;
-- per-session ownership (sessions.owner_instance → processes) makes recovery
-- owner-scoped so a live peer's in-flight sessions are never stomped.

-- ═══ Global settings ═══
-- Registry-backed (src/config_registry.c): default_value + description are
-- code-owned and resynced every startup; value is the operator/agent override
-- (NULL = use default). Effective value = COALESCE(value, default_value).
CREATE TABLE IF NOT EXISTS config (
  key           TEXT PRIMARY KEY,
  value         TEXT,
  default_value TEXT,
  description   TEXT
);

-- ═══ Providers ═══
CREATE TABLE IF NOT EXISTS providers (
  name TEXT PRIMARY KEY,
  base_url TEXT NOT NULL,
  endpoint_type TEXT NOT NULL DEFAULT 'openai',
  api_key_env TEXT NOT NULL DEFAULT '',
  default_model TEXT,
  context_window INTEGER DEFAULT 128000,
  priority INTEGER NOT NULL DEFAULT 0,
  status TEXT NOT NULL DEFAULT 'healthy'
);

-- ═══ Models ═══
CREATE TABLE IF NOT EXISTS models (
  id TEXT PRIMARY KEY,
  provider_name TEXT NOT NULL,
  model TEXT NOT NULL,
  sub_provider TEXT,
  context_window INTEGER DEFAULT 128000,
  max_output_tokens INTEGER,
  capabilities TEXT DEFAULT '[]',
  priority INTEGER NOT NULL DEFAULT 0,
  status TEXT NOT NULL DEFAULT 'healthy',
  degraded_until INTEGER,
  total_requests INTEGER DEFAULT 0,
  total_tokens_in INTEGER DEFAULT 0,
  total_tokens_out INTEGER DEFAULT 0,
  total_cost_nano INTEGER DEFAULT 0,
  error_count_5xx INTEGER DEFAULT 0,
  error_count_429 INTEGER DEFAULT 0,
  last_success_at INTEGER,
  last_error_at INTEGER,
  synced_at INTEGER,
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE INDEX IF NOT EXISTS idx_models_routing ON models(priority, status);

-- ═══ LLM Jobs (transient queue) ═══
CREATE TABLE IF NOT EXISTS llm_jobs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL,
  agent_name TEXT NOT NULL DEFAULT 'default',
  recall INTEGER NOT NULL DEFAULT 0,
  job_type INTEGER NOT NULL DEFAULT 0,
  status TEXT NOT NULL DEFAULT 'pending',
  claimed_at INTEGER,
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE INDEX IF NOT EXISTS idx_llm_jobs_pending ON llm_jobs(status) WHERE status='pending';

-- ═══ Extensions ═══
CREATE TABLE IF NOT EXISTS extensions (
  name TEXT PRIMARY KEY,
  path TEXT NOT NULL,                       -- shared store dir: ~/.cclaw/extensions/<name>
  version TEXT DEFAULT '0.0.0',
  owner_agent TEXT,                         -- who promoted it (NULL for builtin)
  published INTEGER NOT NULL DEFAULT 0,     -- the single publish flag
  builtin INTEGER NOT NULL DEFAULT 0,
  enabled INTEGER NOT NULL DEFAULT 1,
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);

-- ═══ Agents ═══
CREATE TABLE IF NOT EXISTS agents (
  name TEXT PRIMARY KEY,
  model TEXT,
  provider TEXT,
  system_prompt TEXT,
  description TEXT,
  max_iterations INTEGER DEFAULT 25,
  max_output_tokens INTEGER,
  shell_timeout INTEGER DEFAULT 30,
  shell_path TEXT,               -- interpreter for shell_exec's -c; NULL = /bin/sh
  sandbox_profile TEXT DEFAULT 'standard',
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);

-- ═══ Grants (normalized capabilities) ═══
-- approval_mode applies only to kind='tool' rows (Axis B of §4 authority):
--   'silent'       — granted tool runs freely (default)
--   'always'       — every call parks for approval
--   'tool_decides' — predicate decides; absent a predicate, fail-closed to 'always'
CREATE TABLE IF NOT EXISTS grants (
  agent_name TEXT NOT NULL,
  kind TEXT NOT NULL,
  value TEXT NOT NULL,
  approval_mode TEXT NOT NULL DEFAULT 'silent',
  expires_at INTEGER,
  created_at INTEGER DEFAULT (unixepoch()),
  PRIMARY KEY (agent_name, kind, value)
);

CREATE TABLE IF NOT EXISTS agent_extensions (
  agent_name TEXT,
  extension_name TEXT NOT NULL,
  config TEXT,
  enabled INTEGER NOT NULL DEFAULT 1,
  PRIMARY KEY (agent_name, extension_name)
);

-- ═══ Channels ═══
-- status lifecycle: draft → validated → active → broken. extension_install()
-- inserts 'draft'; only `cclaw --channel <name> --check` (→validated) then
-- `--activate` (→active) can reach 'active' — channel_launch_all() only execs
-- that state. channel_reap()'s flap detection writes 'broken' on crash-loop;
-- another --check retries validated→active.
CREATE TABLE IF NOT EXISTS channels (
  name TEXT PRIMARY KEY,
  extension_name TEXT NOT NULL DEFAULT '',
  type TEXT NOT NULL DEFAULT '',
  binary_path TEXT NOT NULL DEFAULT '',
  status TEXT NOT NULL DEFAULT 'draft',
  pid INTEGER,
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);

CREATE TABLE IF NOT EXISTS channel_state (
  channel_name TEXT NOT NULL,
  key TEXT NOT NULL,
  value TEXT,
  PRIMARY KEY (channel_name, key)
);

CREATE TABLE IF NOT EXISTS channel_routes (
  channel_name TEXT NOT NULL,
  channel_id TEXT NOT NULL DEFAULT '*',
  agent_name TEXT,
  session_id INTEGER,
  PRIMARY KEY (channel_name, channel_id)
);

-- ═══ Sessions ═══
CREATE TABLE IF NOT EXISTS sessions (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT,
  agent_name TEXT,
  channel_name TEXT,
  channel_id TEXT,
  parent_session_id INTEGER DEFAULT -1,
  parent_tool_call_id TEXT,
  depth INTEGER NOT NULL DEFAULT 0,
  tool_filter TEXT,                         -- JSON array of tool names; NULL = unrestricted.
                                            -- Positive scope frozen at spawn, intersected with
                                            -- grants per turn — never grants anything itself.
  state TEXT NOT NULL DEFAULT 'idle',
  owner_instance TEXT,                      -- live owner (processes.instance_id); NULL ⟺ state='idle'
  turn_iteration INTEGER NOT NULL DEFAULT 0,
  leaf_id INTEGER DEFAULT -1,
  last_route TEXT,
  last_interaction_id TEXT,
  last_synced_entry_id INTEGER,
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  updated_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE INDEX IF NOT EXISTS idx_sessions_owner ON sessions(owner_instance) WHERE owner_instance IS NOT NULL;

-- ═══ Entries ═══
CREATE TABLE IF NOT EXISTS entries (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL,
  parent_id INTEGER NOT NULL DEFAULT -1,
  original_parent_id INTEGER,
  turn_id INTEGER,
  type TEXT NOT NULL DEFAULT 'user_message',
  part_index INTEGER NOT NULL DEFAULT 0,
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
  cost_nano INTEGER,
  token_estimate INTEGER,
  content_bytes INTEGER,
  tool_call_count INTEGER NOT NULL DEFAULT 0,
  data TEXT,
  network_hosts TEXT,  -- NULL or JSON array of hosts the tool run contacted
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE INDEX IF NOT EXISTS idx_entries_session ON entries(session_id, id);
CREATE INDEX IF NOT EXISTS idx_entries_parent ON entries(parent_id);
CREATE INDEX IF NOT EXISTS idx_entries_session_role ON entries(session_id, role);
CREATE INDEX IF NOT EXISTS idx_entries_turn ON entries(session_id, turn_id);
CREATE INDEX IF NOT EXISTS idx_entries_turn_type ON entries(session_id, turn_id, type, part_index);
CREATE INDEX IF NOT EXISTS idx_entries_stop_reason ON entries(session_id, stop_reason) WHERE stop_reason != 0;
CREATE INDEX IF NOT EXISTS idx_entries_plan ON entries(parent_id, session_id, id, role, stop_reason, token_estimate, tool_call_count);

CREATE VIRTUAL TABLE IF NOT EXISTS entries_fts USING fts5(
  content, content=entries, content_rowid=id,
  tokenize='porter unicode61'
);
CREATE TRIGGER IF NOT EXISTS entries_ai AFTER INSERT ON entries BEGIN
  INSERT INTO entries_fts(rowid, content) VALUES (
    new.id,
    CASE new.type
      WHEN 'tool_call' THEN COALESCE(new.tool_name,'') || ' ' || COALESCE(new.content,'')
      WHEN 'tool_result' THEN COALESCE(new.tool_name,'') || ' ' || COALESCE(new.content,'')
      ELSE COALESCE(new.content,'')
    END
  );
END;

-- Leaf advance is atomic with insert; compaction excluded (entry_compact inserts mid-branch)
CREATE TRIGGER IF NOT EXISTS entries_leaf_ai AFTER INSERT ON entries
WHEN NEW.type != 'compaction'
BEGIN
  UPDATE sessions SET leaf_id = NEW.id, updated_at = unixepoch() WHERE id = NEW.session_id;
END;

-- ═══ Tool calls ═══
CREATE TABLE IF NOT EXISTS tool_calls (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL,
  entry_id INTEGER NOT NULL,
  call_id TEXT NOT NULL,
  name TEXT NOT NULL,
  arguments TEXT,
  result_entry_id INTEGER,
  status TEXT NOT NULL DEFAULT 'pending',
  resolved_by TEXT,
  resolved_at INTEGER
);
CREATE INDEX IF NOT EXISTS idx_tool_calls_entry ON tool_calls(entry_id);
CREATE INDEX IF NOT EXISTS idx_tool_calls_session ON tool_calls(session_id, status);

-- ═══ Raw LLM responses (forensics / shape archive) ═══
-- One row per LLM response (any HTTP outcome). Retention is set by config key
-- 'llm_response_archive_max': >0 keeps the most recent N, 0 disables archiving,
-- <0 keeps all. body holds the parsed JSONB blob when the response is valid
-- JSON, or the raw text when it isn't; turn_id joins back to entries.
CREATE TABLE IF NOT EXISTS llm_responses (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL,
  turn_id INTEGER NOT NULL,
  model TEXT,
  status TEXT NOT NULL,          -- ok | empty | malformed | http_<code> | timeout | network_error
  provider_id TEXT,              -- provider's own response id ($.id), NULL if absent
  body BLOB,                     -- JSONB when parseable, raw text otherwise
  request_body BLOB,             -- JSONB of the payload we sent (failures only); NULL on success
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE INDEX IF NOT EXISTS idx_llm_responses_turn ON llm_responses(session_id, turn_id);

-- ═══ IPC ═══
CREATE TABLE IF NOT EXISTS channel_events (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  channel_name TEXT NOT NULL,
  event_type TEXT NOT NULL DEFAULT 'message',
  payload TEXT NOT NULL,
  external_id TEXT,
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  UNIQUE(channel_name, external_id)
);

CREATE TABLE IF NOT EXISTS channel_outbox (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  channel_name TEXT NOT NULL,
  session_id INTEGER NOT NULL,
  payload TEXT NOT NULL,
  status TEXT NOT NULL DEFAULT 'pending',
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  acked_at INTEGER
);
CREATE INDEX IF NOT EXISTS idx_channel_outbox_pending ON channel_outbox(channel_name, status) WHERE status='pending';

CREATE TABLE IF NOT EXISTS inbox (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL,
  source TEXT NOT NULL DEFAULT 'cli',
  payload TEXT NOT NULL,
  consumed INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE INDEX IF NOT EXISTS idx_inbox_pending ON inbox(session_id, consumed) WHERE consumed = 0;

-- ═══ Tools ═══
CREATE TABLE IF NOT EXISTS tools (
  name TEXT PRIMARY KEY,
  extension_name TEXT,                       -- NULL for built-in C tools
  description TEXT,
  parameters_json TEXT,
  path TEXT,                                 -- handler file (in the shared store)
  builtin INTEGER NOT NULL DEFAULT 0,
  agent_name TEXT,                           -- owner scope, NULL = global
  enabled INTEGER NOT NULL DEFAULT 1,
  policy TEXT
);

-- ═══ Hooks ═══
-- Same provenance model as tools (replaces the __cclaw_hooks scrape).
CREATE TABLE IF NOT EXISTS hooks (
  extension_name TEXT NOT NULL,
  event TEXT NOT NULL,                        -- one of the six hook events
  path TEXT NOT NULL,                         -- handler file (in the shared store)
  enabled INTEGER NOT NULL DEFAULT 1,
  PRIMARY KEY (extension_name, event, path)
);

-- Per-request hook directives: preAdvance commands that must cross the
-- main-thread → worker-thread boundary as DB state. 'inject' (ephemeral) and
-- 'suppress' rows live for exactly one LLM request — written at dispatch on
-- the poll thread, read by the worker's payload build, deleted at llm_req exit.
CREATE TABLE IF NOT EXISTS hook_directives (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL,
  kind TEXT NOT NULL,              -- 'inject' | 'suppress'
  role TEXT,                       -- inject: 'system' | 'user'
  content TEXT,                    -- inject payload
  entry_id INTEGER,                -- suppress target
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE INDEX IF NOT EXISTS idx_hook_directives_session ON hook_directives(session_id);

-- ═══ Memory ═══
CREATE TABLE IF NOT EXISTS memory_blocks (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  agent_name TEXT,
  label TEXT NOT NULL,
  value TEXT NOT NULL DEFAULT '',
  description TEXT,
  char_limit INTEGER NOT NULL DEFAULT 5000,
  read_only INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  updated_at INTEGER NOT NULL DEFAULT (unixepoch()),
  UNIQUE(agent_name, label)
);

-- Numbered entries within a memory block (block = container of entries)
CREATE TABLE IF NOT EXISTS memory_entries (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  agent_name TEXT NOT NULL,
  block_label TEXT NOT NULL,
  pos INTEGER NOT NULL,
  text TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE INDEX IF NOT EXISTS idx_memory_entries_block
  ON memory_entries(agent_name, block_label, pos);

-- ═══ Cron ═══
CREATE TABLE IF NOT EXISTS cron_jobs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  agent_name TEXT,
  name TEXT NOT NULL,
  cron_expr TEXT NOT NULL,
  session_id INTEGER NOT NULL,
  task TEXT NOT NULL,
  enabled INTEGER NOT NULL DEFAULT 1,
  next_run_at INTEGER NOT NULL DEFAULT 0,
  last_run_at INTEGER,
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);


-- ═══ Approvals ═══
-- approvals is history/audit; expires_at is the park deadline (fail-closed deny).
-- grants is live truth; grants.expires_at is the time-bounded-grant feature.
-- They do not overlap now that once-scope is gone.
CREATE TABLE IF NOT EXISTS approvals (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id   INTEGER NOT NULL,
  tool_call_id TEXT,
  tool_name    TEXT,
  action       TEXT,
  args_json    TEXT,
  resolve      TEXT NOT NULL DEFAULT 'rerun',
  state        TEXT NOT NULL DEFAULT 'pending',
  decided_via  TEXT,
  requested_at INTEGER DEFAULT (unixepoch()),
  expires_at   INTEGER
);
CREATE INDEX IF NOT EXISTS approvals_pending ON approvals(session_id, state) WHERE state='pending';

-- ═══ Sensitivity axis (specs/trust.md) ═══
-- Operator-owned labels on TARGETS, global (not per-agent). The one place a
-- label subtracts from authority: a sensitive host always escalates, and no
-- standing grant satisfies it — the proxy deny-checks these before any allow
-- rule, and dispatch parks every matching call (ALWAYS coerced to ONCE).
CREATE TABLE IF NOT EXISTS sensitive_targets (
  kind  TEXT NOT NULL DEFAULT 'host',        -- only 'host' in v1
  value TEXT NOT NULL,                       -- exact host or '.suffix' rule
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  PRIMARY KEY (kind, value)
);

-- Fail-closed credential rule (specs/trust.md): a secret may only be
-- submitted to hosts it is bound to. First use of a loaded secret with no
-- bindings always parks; ALWAYS on that approval records the binding
-- ("approve & bind"). For shell/js calls carrying a secret, the call's
-- egress allow-list IS the union of the used secrets' bound hosts.
CREATE TABLE IF NOT EXISTS secret_hosts (
  secret_name TEXT NOT NULL,
  host        TEXT NOT NULL,                 -- exact host or '.suffix' rule
  created_at  INTEGER NOT NULL DEFAULT (unixepoch()),
  PRIMARY KEY (secret_name, host)
);

-- Secret store (specs/security.md): DB-backed secrets, born via the
-- `cclaw secret set` operator verb or the secret_create tool. value is always
-- "enc:<hex(...)>" (never plaintext). status='pending' is provenance/UX only —
-- enforcement comes free from secret_hosts having zero rows for a new secret
-- (first use always parks; ALWAYS-approval binds and flips it to 'active').
-- scope: 'agent' secrets feed {{SECRET:name}} interpolation + child injection;
-- 'system' secrets (provider API keys) are daemon-consumed only and are never
-- loaded into the agent-facing snapshot — an agent cannot interpolate them.
CREATE TABLE IF NOT EXISTS secrets (
  name       TEXT PRIMARY KEY,                    -- ^[A-Z][A-Z0-9_]*$
  value      TEXT NOT NULL,
  status     TEXT NOT NULL DEFAULT 'active',       -- 'active' | 'pending'
  source     TEXT NOT NULL DEFAULT 'operator',      -- 'operator'|'generated'|'quarantine'
  scope      TEXT NOT NULL DEFAULT 'agent'
             CHECK (scope IN ('agent','system')),
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);

-- ═══ Process registry (liveness) ═══
-- One row per live cclaw process (daemon or cli) sharing this DB. Sessions stamp
-- their owner_instance here; owner-scoped recovery reclaims only sessions whose
-- owner is absent (crashed) or stale (no heartbeat within PROCESS_TTL_SEC).
CREATE TABLE IF NOT EXISTS processes (
  instance_id  TEXT PRIMARY KEY,
  pid          INTEGER NOT NULL,
  mode         TEXT NOT NULL,                 -- 'daemon' | 'cli'
  started_at   INTEGER NOT NULL DEFAULT (unixepoch()),
  heartbeat_at INTEGER NOT NULL DEFAULT (unixepoch())
);
