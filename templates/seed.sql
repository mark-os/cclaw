-- Default seed data for fresh cclaw.db installations.
-- Executed once when config table is empty.

-- ═══ Config defaults ═══
INSERT OR IGNORE INTO config(key, value) VALUES
  ('default_model',         'deepseek/deepseek-v4-flash'),
  ('default_provider',      'openrouter'),
  ('max_iterations',        '25'),
  ('shell_timeout',         '30'),
  ('web_port',              '8080'),
  ('default_allowed_tools', '["file_read","file_write","js_eval","request_config"]'),
  ('health_5xx_threshold',  '3'),
  ('health_429_threshold',  '10'),
  ('health_window_sec',     '300'),
  ('health_cooldown_sec',   '300');

-- ═══ Default provider ═══
INSERT OR IGNORE INTO providers(name, base_url, endpoint_type, api_key_env, default_model, priority)
  VALUES('openrouter', 'https://openrouter.ai/api/v1', 'openai', 'OPENROUTER_API_KEY', 'deepseek/deepseek-v4-flash', 0);

-- ═══ Default model ═══
INSERT OR IGNORE INTO models(id, provider_name, model, context_window, priority)
  VALUES('openrouter/deepseek/deepseek-v4-flash', 'openrouter', 'deepseek/deepseek-v4-flash', 128000, 0);

-- ═══ Built-in tools ═══
INSERT OR IGNORE INTO tools(name, description, parameters_json) VALUES
  ('file_read', 'Read a file within the workspace directory',
   '{"type":"object","properties":{"path":{"type":"string","description":"File path to read (relative to workspace)"}},"required":["path"]}');
INSERT OR IGNORE INTO tools(name, description, parameters_json) VALUES
  ('file_write', 'Write content to a file within the workspace directory',
   '{"type":"object","properties":{"path":{"type":"string","description":"File path to write (relative to workspace)"},"content":{"type":"string","description":"Content to write"}},"required":["path","content"]}');
INSERT OR IGNORE INTO tools(name, description, parameters_json) VALUES
  ('shell_exec', 'Execute a shell command in a sandboxed environment',
   '{"type":"object","properties":{"command":{"type":"string","description":"Shell command to execute"},"timeout":{"type":"integer","description":"Timeout in seconds (default 30)"}},"required":["command"]}');
INSERT OR IGNORE INTO tools(name, description, parameters_json) VALUES
  ('js_eval', 'Evaluate JavaScript code using the embedded QuickJS engine',
   '{"type":"object","properties":{"code":{"type":"string","description":"JavaScript code to evaluate"}},"required":["code"]}');
INSERT OR IGNORE INTO tools(name, description, parameters_json) VALUES
  ('web_fetch', 'Fetch content from a URL',
   '{"type":"object","properties":{"url":{"type":"string","description":"URL to fetch"},"method":{"type":"string","description":"HTTP method (default GET)"}},"required":["url"]}');
INSERT OR IGNORE INTO tools(name, description, parameters_json) VALUES
  ('memory_create', 'Create a persistent memory block',
   '{"type":"object","properties":{"label":{"type":"string","description":"Memory block label"},"content":{"type":"string","description":"Initial content"}},"required":["label","content"]}');
INSERT OR IGNORE INTO tools(name, description, parameters_json) VALUES
  ('memory_append', 'Append text to a memory block',
   '{"type":"object","properties":{"label":{"type":"string","description":"Memory block label"},"content":{"type":"string","description":"Content to append"}},"required":["label","content"]}');
INSERT OR IGNORE INTO tools(name, description, parameters_json) VALUES
  ('memory_replace', 'Replace content of a memory block',
   '{"type":"object","properties":{"label":{"type":"string","description":"Memory block label"},"content":{"type":"string","description":"New content"}},"required":["label","content"]}');
INSERT OR IGNORE INTO tools(name, description, parameters_json) VALUES
  ('request_config', 'Request a configuration change from the user',
   '{"type":"object","properties":{"key":{"type":"string","description":"Config key to set"},"value":{"type":"string","description":"Config value"}},"required":["key","value"]}');
INSERT OR IGNORE INTO tools(name, description, parameters_json) VALUES
  ('launch_agent', 'Launch a sub-agent to perform a task',
   '{"type":"object","properties":{"task":{"type":"string","description":"Task description for the sub-agent"},"name":{"type":"string","description":"Name of existing agent to launch (omit for default)"},"background":{"type":"boolean","description":"Run in background (default: false, blocks until done)"}},"required":["task"]}');
INSERT OR IGNORE INTO tools(name, description, parameters_json) VALUES
  ('check_agent', 'Check a background sub-agent status and result',
   '{"type":"object","properties":{"session_id":{"type":"integer","description":"Session ID of the sub-agent to check"}},"required":["session_id"]}');
