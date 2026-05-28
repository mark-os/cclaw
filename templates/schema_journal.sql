CREATE TABLE IF NOT EXISTS log (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  source TEXT NOT NULL,
  pid INTEGER,
  session_id INTEGER,
  stream INTEGER NOT NULL DEFAULT 1,
  line TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE INDEX IF NOT EXISTS idx_log_source ON log(source, created_at);
CREATE INDEX IF NOT EXISTS idx_log_time ON log(created_at);
