-- Default seed data for fresh cclaw.db installations.
-- Executed once when the providers table is empty.
--
-- No config rows here: every config key lives in the C registry
-- (src/config_registry.c) with a default and description, synced into the
-- config table at startup. Seed data is only for genuinely row-shaped data
-- (providers, models, tool descriptions).

-- ═══ Default provider ═══
INSERT OR IGNORE INTO providers(name, base_url, endpoint_type, api_key_env, default_model, priority)
  VALUES('openrouter', 'https://openrouter.ai/api/v1', 'openai', 'OPENROUTER_API_KEY', 'deepseek/deepseek-v4-flash', 0);

-- ═══ Default model ═══
INSERT OR IGNORE INTO models(id, provider_name, model, priority)
  VALUES('openrouter/deepseek/deepseek-v4-flash', 'openrouter', 'deepseek/deepseek-v4-flash', 0);

-- ═══ Built-in tools ═══
-- Descriptions are a write-once seed: tools_sync_to_db() upserts with
-- COALESCE(tools.description, excluded.description), so on a fresh DB these
-- rows win and the C-registered description is never consulted again. Keep
-- them in sync with the tools_register() call sites in src/tool_*.c.
INSERT OR IGNORE INTO tools(name, description) VALUES
  ('file_read', 'Read a file (path relative or absolute)'),
  ('file_write', 'Write content to a file (path relative or absolute)'),
  ('shell_exec', 'Execute a shell command and return stdout+stderr'),
  ('js_eval', 'Run JavaScript in QuickJS (modern JS; no modules, no top-level await). http_request(url[, {method, body, headers:{Name:Value}, sanitize}]) is synchronous HTTP; returns {status, body} where body is a string ('''' on error, plus {error}). Use .body, e.g. http_request(url).body.match(/re/).'),
  ('web_fetch', 'Fetch a URL via HTTP GET and return content as markdown. Large pages are truncated; page through with offset + max_chars, or read/grep the full copy saved under workspace .tool_results/ (path given in the result).'),
  ('memory_create', 'Create a new memory block (a named container of numbered notes). Args: label, description.'),
  ('memory_add', 'Add a numbered note to a memory block. Args: block (label), text.'),
  ('memory_edit', 'Replace the text of existing notes by number. Args: block, edits (array of {number, text}).'),
  ('memory_delete', 'Delete notes by number (others renumber). Args: block, numbers (array of integers).'),
  ('request_config', 'Request a configuration change (requires human approval).'),
  ('search_config', 'Discover your current configuration and what you can request: your sandbox profile, granted tools and hosts, the full list of available tools, and how to request more via request_config.'),
  ('launch_agent', 'Delegate a task to another agent'),
  ('check_session', 'Check the status and result of a sub-agent session'),
  ('secret_create', 'Mint a new random credential, stored encrypted in the DB. Returns only its {{SECRET:name}} placeholder — never the value.'),
  ('configure_provider', 'Set up LLM provider. Stores the API key encrypted in cclaw.db. Known providers: openrouter, gemini, anthropic.'),
  ('configure_channel', 'Set up a communication channel. Supported: telegram (requires bot_token), cli, or custom (requires binary_path).'),
  ('create_agent', 'Propose creation of a named agent. Requires admin approval. On approval, daemon creates agent directory, seeds DB, and binds to channel.');
