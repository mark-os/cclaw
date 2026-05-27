CREATE TABLE IF NOT EXISTS sessions (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT,
  leaf_id INTEGER DEFAULT -1,
  agent_name TEXT,
  parent_session_id INTEGER DEFAULT -1,
  depth INTEGER NOT NULL DEFAULT 0,
  state TEXT NOT NULL DEFAULT 'idle',
  last_route TEXT,
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  updated_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE TABLE IF NOT EXISTS entries (
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
CREATE INDEX IF NOT EXISTS idx_entries_session ON entries(session_id, id);
CREATE INDEX IF NOT EXISTS idx_entries_parent ON entries(parent_id);
CREATE INDEX IF NOT EXISTS idx_entries_session_role ON entries(session_id, role);
CREATE INDEX IF NOT EXISTS idx_entries_turn ON entries(session_id, turn_id);
CREATE INDEX IF NOT EXISTS idx_entries_stop_reason ON entries(session_id, stop_reason) WHERE stop_reason != 0;
CREATE INDEX IF NOT EXISTS idx_entries_plan ON entries(parent_id, session_id, id, role, stop_reason, token_estimate, tool_call_count);
CREATE VIRTUAL TABLE IF NOT EXISTS entries_fts USING fts5(
  content, content=entries, content_rowid=id
);
CREATE TRIGGER IF NOT EXISTS entries_ai AFTER INSERT ON entries BEGIN
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
CREATE TABLE IF NOT EXISTS kv (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS tg_chat_sessions (
  chat_id INTEGER PRIMARY KEY,
  session_id INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS js_tools (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL,
  name TEXT NOT NULL,
  description TEXT,
  parameters_json TEXT,
  code TEXT NOT NULL,
  UNIQUE(session_id, name)
);
CREATE TABLE IF NOT EXISTS cron_jobs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL,
  cron_expr TEXT NOT NULL,
  session_id INTEGER NOT NULL,
  task TEXT NOT NULL,
  enabled INTEGER NOT NULL DEFAULT 1,
  next_run_at INTEGER NOT NULL DEFAULT 0,
  last_run_at INTEGER,
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE TABLE IF NOT EXISTS inbox (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL,
  source TEXT NOT NULL,
  payload TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  consumed INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_inbox_pending ON inbox(session_id, consumed) WHERE consumed = 0;
CREATE TABLE IF NOT EXISTS spawn_queue (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  parent_session_id INTEGER NOT NULL,
  task TEXT NOT NULL,
  background INTEGER NOT NULL DEFAULT 0,
  depth INTEGER NOT NULL DEFAULT 1,
  tool_call_id TEXT,
  status TEXT NOT NULL DEFAULT 'pending',
  child_session_id INTEGER,
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE INDEX IF NOT EXISTS idx_spawn_queue_pending ON spawn_queue(status) WHERE status='pending';
CREATE TABLE IF NOT EXISTS agents (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL UNIQUE,
  config TEXT,
  system_prompt TEXT,
  heartbeat TEXT,
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  updated_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE TABLE IF NOT EXISTS approvals (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL,
  agent_name TEXT NOT NULL,
  type TEXT NOT NULL,
  payload TEXT NOT NULL,
  status TEXT NOT NULL DEFAULT 'pending',
  admin_chat_id INTEGER,
  notified INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  resolved_at INTEGER
);
CREATE INDEX IF NOT EXISTS idx_approvals_pending ON approvals(status) WHERE status='pending';
CREATE TABLE IF NOT EXISTS channel_bindings (
  channel_type TEXT NOT NULL,
  channel_id TEXT NOT NULL,
  agent_name TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  updated_at INTEGER NOT NULL DEFAULT (unixepoch()),
  PRIMARY KEY (channel_type, channel_id)
);
CREATE TABLE IF NOT EXISTS memory_blocks (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  agent_name TEXT NOT NULL,
  label TEXT NOT NULL,
  value TEXT NOT NULL DEFAULT '',
  description TEXT,
  char_limit INTEGER NOT NULL DEFAULT 5000,
  read_only INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  updated_at INTEGER NOT NULL DEFAULT (unixepoch()),
  UNIQUE(agent_name, label)
);
