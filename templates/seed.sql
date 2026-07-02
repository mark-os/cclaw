-- Default seed data for fresh cclaw.db installations.
-- Executed once when config table is empty.

-- ═══ Config defaults ═══
INSERT OR IGNORE INTO config(key, value) VALUES
  ('default_model',         'deepseek/deepseek-v4-flash'),
  ('default_provider',      'openrouter'),
  ('max_iterations',        '25'),
  ('shell_timeout',         '30'),
  ('web_port',              '8080'),
  ('worker_tools',          '["file_read","file_write","shell_exec","web_fetch","js_eval","check_session","check_approval","search_config"]'),
  ('agent_default_tools',   '["file_read","file_write","js_eval","request_config","search_config","memory_create","memory_add","memory_edit","memory_delete","configure_provider","configure_channel","create_agent","extension_promote","extension_publish","extension_attach","extension_list","launch_agent","check_session","check_approval"]'),
  ('health_5xx_threshold',  '3'),
  ('health_429_threshold',  '10'),
  ('health_window_sec',     '300'),
  ('health_cooldown_sec',   '300'),
  ('approval_block_sec',    '60');

-- ═══ Default provider ═══
INSERT OR IGNORE INTO providers(name, base_url, endpoint_type, api_key_env, default_model, priority)
  VALUES('openrouter', 'https://openrouter.ai/api/v1', 'openai', 'OPENROUTER_API_KEY', 'deepseek/deepseek-v4-flash', 0);

-- ═══ Default model ═══
INSERT OR IGNORE INTO models(id, provider_name, model, context_window, priority)
  VALUES('openrouter/deepseek/deepseek-v4-flash', 'openrouter', 'deepseek/deepseek-v4-flash', 128000, 0);

-- ═══ Built-in tools ═══
INSERT OR IGNORE INTO tools(name, description) VALUES
  ('file_read', 'Read a file (path relative or absolute)'),
  ('file_write', 'Write content to a file (path relative or absolute)'),
  ('shell_exec', 'Execute a shell command in a sandboxed environment'),
  ('js_eval', 'Evaluate JavaScript code using the embedded QuickJS engine'),
  ('web_fetch', 'Fetch content from a URL'),
  ('memory_create', 'Create a memory block (named container of numbered notes)'),
  ('memory_add', 'Add a numbered note to a memory block'),
  ('memory_edit', 'Edit notes in a memory block by number'),
  ('memory_delete', 'Delete notes from a memory block by number'),
  ('request_config', 'Request a configuration change from the user'),
  ('search_config', 'Discover current config, available tools, and how to request changes'),
  ('launch_agent', 'Launch a sub-agent to perform a task'),
  ('check_agent', 'Check a background sub-agent status and result');
