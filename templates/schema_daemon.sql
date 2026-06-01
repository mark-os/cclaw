CREATE TABLE IF NOT EXISTS agents (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL UNIQUE,
  config TEXT,
  system_prompt TEXT,
  heartbeat TEXT,
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  updated_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE TABLE IF NOT EXISTS agent_config (
  agent_name TEXT NOT NULL,
  key TEXT NOT NULL,
  value TEXT NOT NULL,
  PRIMARY KEY (agent_name, key)
);
CREATE TABLE IF NOT EXISTS providers (
  name TEXT PRIMARY KEY,
  base_url TEXT NOT NULL,
  endpoint_type TEXT NOT NULL DEFAULT 'openai',
  api_key_env TEXT NOT NULL DEFAULT '',
  default_model_id TEXT,
  context_window INTEGER DEFAULT 128000
);
CREATE TABLE IF NOT EXISTS kv (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS channel_bindings (
  channel_type TEXT NOT NULL,
  channel_id TEXT NOT NULL,
  agent_name TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  updated_at INTEGER NOT NULL DEFAULT (unixepoch()),
  PRIMARY KEY (channel_type, channel_id)
);
CREATE TABLE IF NOT EXISTS tg_chat_sessions (
  chat_id INTEGER PRIMARY KEY,
  session_id INTEGER NOT NULL,
  agent_name TEXT NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS spawn_queue (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  parent_agent TEXT NOT NULL DEFAULT '',
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
CREATE INDEX IF NOT EXISTS idx_spawn_queue_pending ON spawn_queue(status) WHERE status='pending';
CREATE TABLE IF NOT EXISTS cron_jobs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  agent_name TEXT NOT NULL DEFAULT '',
  name TEXT NOT NULL,
  cron_expr TEXT NOT NULL,
  session_id INTEGER NOT NULL,
  task TEXT NOT NULL,
  enabled INTEGER NOT NULL DEFAULT 1,
  next_run_at INTEGER NOT NULL DEFAULT 0,
  last_run_at INTEGER,
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE TABLE IF NOT EXISTS approvals (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  agent_name TEXT NOT NULL,
  session_id INTEGER NOT NULL,
  tool_call_id TEXT,
  type TEXT NOT NULL,
  payload TEXT NOT NULL,
  status TEXT NOT NULL DEFAULT 'pending',
  admin_chat_id INTEGER,
  notified INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  resolved_at INTEGER
);
CREATE INDEX IF NOT EXISTS idx_approvals_pending ON approvals(status) WHERE status='pending';
CREATE TABLE IF NOT EXISTS channels (
  name TEXT PRIMARY KEY,
  type TEXT NOT NULL,
  binary_path TEXT NOT NULL,
  status TEXT NOT NULL DEFAULT 'active',
  pid INTEGER,
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE TABLE IF NOT EXISTS channel_events (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  channel_name TEXT NOT NULL,
  event_type TEXT NOT NULL,
  payload TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
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
CREATE TABLE IF NOT EXISTS channel_state (
  channel_name TEXT NOT NULL,
  key TEXT NOT NULL,
  value TEXT NOT NULL,
  PRIMARY KEY (channel_name, key)
);
