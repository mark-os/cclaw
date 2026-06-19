-- cclaw.db unified schema
-- Single file, WAL mode. Daemon is primary writer for coordination tables.

-- ═══ Global settings ═══
CREATE TABLE IF NOT EXISTS config (
  key TEXT PRIMARY KEY,
  value TEXT
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
  path TEXT NOT NULL,
  version TEXT DEFAULT '0.0.0',
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
  max_iterations INTEGER DEFAULT 25,
  max_output_tokens INTEGER,
  shell_timeout INTEGER DEFAULT 30,
  trust_level TEXT DEFAULT 'standard',
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);

-- ═══ Grants (normalized capabilities) ═══
CREATE TABLE IF NOT EXISTS grants (
  agent_name TEXT NOT NULL,
  kind TEXT NOT NULL,
  value TEXT NOT NULL,
  expires_at INTEGER,
  created_at INTEGER DEFAULT (unixepoch()),
  PRIMARY KEY (agent_name, kind, value)
);

CREATE TABLE IF NOT EXISTS agent_extensions (
  agent_name TEXT,
  extension_name TEXT NOT NULL,
  config TEXT,
  PRIMARY KEY (agent_name, extension_name)
);

-- ═══ Channels ═══
CREATE TABLE IF NOT EXISTS channels (
  name TEXT PRIMARY KEY,
  extension_name TEXT NOT NULL DEFAULT '',
  type TEXT NOT NULL DEFAULT '',
  binary_path TEXT NOT NULL DEFAULT '',
  status TEXT NOT NULL DEFAULT 'active',
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
  state TEXT NOT NULL DEFAULT 'idle',
  turn_iteration INTEGER NOT NULL DEFAULT 0,
  leaf_id INTEGER DEFAULT -1,
  last_route TEXT,
  cache_break_after INTEGER DEFAULT -1,
  last_interaction_id TEXT,
  last_synced_entry_id INTEGER,
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  updated_at INTEGER NOT NULL DEFAULT (unixepoch())
);

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
  description TEXT,
  parameters_json TEXT,
  path TEXT,
  builtin INTEGER NOT NULL DEFAULT 1,
  agent_name TEXT,
  enabled INTEGER NOT NULL DEFAULT 1
);



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
  args_hash    TEXT,
  state        TEXT NOT NULL DEFAULT 'pending',
  decided_via  TEXT,
  requested_at INTEGER DEFAULT (unixepoch()),
  expires_at   INTEGER
);
CREATE INDEX IF NOT EXISTS approvals_pending ON approvals(session_id, state) WHERE state='pending';
