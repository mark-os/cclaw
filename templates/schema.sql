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
  turn_id INTEGER,
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  data TEXT NOT NULL,
  token_estimate INTEGER,
  content_bytes INTEGER,
  type TEXT GENERATED ALWAYS AS (json_extract(data, '$.type')) STORED,
  role TEXT GENERATED ALWAYS AS (json_extract(data, '$.role')) STORED
);
CREATE INDEX IF NOT EXISTS idx_entries_session ON entries(session_id, id);
CREATE INDEX IF NOT EXISTS idx_entries_parent ON entries(parent_id);
CREATE INDEX IF NOT EXISTS idx_entries_session_type ON entries(session_id, type);
CREATE INDEX IF NOT EXISTS idx_entries_session_role ON entries(session_id, role);
CREATE INDEX IF NOT EXISTS idx_entries_turn ON entries(session_id, turn_id);
CREATE VIRTUAL TABLE IF NOT EXISTS entries_fts USING fts5(
  content, content=entries, content_rowid=id
);
CREATE TRIGGER IF NOT EXISTS entries_ai AFTER INSERT ON entries BEGIN
  INSERT INTO entries_fts(rowid, content) VALUES (
    new.id,
    CASE json_extract(new.data, '$.role')
      WHEN 'user' THEN json_extract(new.data, '$.content')
      WHEN 'system' THEN json_extract(new.data, '$.content')
      WHEN 'tool_result' THEN COALESCE(json_extract(new.data, '$.name'),'') || ' ' || COALESCE(json_extract(new.data, '$.content'),'')
      WHEN 'assistant' THEN (
        SELECT group_concat(txt, ' ') FROM (
          SELECT CASE json_extract(j.value, '$.type')
            WHEN 'text' THEN json_extract(j.value, '$.text')
            WHEN 'tool_call' THEN json_extract(j.value, '$.name') || ' ' || json_extract(j.value, '$.arguments')
          END AS txt
          FROM json_each(json_extract(new.data, '$.content')) j
        )
      )
      ELSE COALESCE(json_extract(new.data, '$.summary'),'')
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
  soul TEXT,
  memory TEXT,
  heartbeat TEXT,
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  updated_at INTEGER NOT NULL DEFAULT (unixepoch())
);
