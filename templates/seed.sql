-- Default seed data for fresh cclaw.db installations.
-- Executed once when the providers table is empty.
--
-- No config rows here: every config key lives in the C registry
-- (src/config_registry.c) with a default and description, synced into the
-- config table at startup. Seed data is only for genuinely row-shaped data
-- (providers, models, tool descriptions).

-- ═══ Default provider ═══
-- request_extra: StreamLake keeps returning DeepSeek tool calls as raw DSML
-- markup inside `reasoning` (two prod incidents, 2026-08-10 / 2026-08-19), so
-- the seed routes around it with OpenRouter's documented provider params.
INSERT OR IGNORE INTO providers(name, base_url, endpoint_type, api_key_env, default_model, priority, request_extra)
  VALUES('openrouter', 'https://openrouter.ai/api/v1', 'openai', 'OPENROUTER_API_KEY', 'deepseek/deepseek-v4-flash', 0,
         '{"provider":{"ignore":["StreamLake"]}}');

-- ═══ Default model ═══
INSERT OR IGNORE INTO models(id, provider_name, model)
  VALUES('openrouter/deepseek/deepseek-v4-flash', 'openrouter', 'deepseek/deepseek-v4-flash');

-- ═══ Media preprocessing model (capability-routed, e.g. voice transcription) ═══
-- Native Gemini endpoint (llm_build_url appends /models/<model>:generateContent).
-- Chat routing never picks it unless some agent lists it; the audio
-- capability is what selects it (model_pick_by_capability).
INSERT OR IGNORE INTO providers(name, base_url, endpoint_type, api_key_env, default_model, priority)
  VALUES('gemini', 'https://generativelanguage.googleapis.com/v1beta', 'gemini', 'GEMINI_API_KEY', 'gemini-3.5-flash-lite', 50);
INSERT OR IGNORE INTO models(id, provider_name, model, capabilities)
  VALUES('gemini/gemini-3.5-flash-lite', 'gemini', 'gemini-3.5-flash-lite', '["text","image","audio"]');

-- ═══ Known providers (no keys shipped — inert until their key exists) ═══
-- Endpoints sourced from the OpenClaw and Hermes default provider catalogs
-- (production OpenAI-compatible chat completions only). Low priority:
-- config_load's key-availability scan promotes one only when its key
-- resolves, and request_config's provider section uses these rows for
-- default fill.
INSERT OR IGNORE INTO providers(name, base_url, endpoint_type, api_key_env, default_model, priority) VALUES
  ('openai',   'https://api.openai.com/v1',      'openai', 'OPENAI_API_KEY',   'gpt-5.4',         100),
  ('deepseek', 'https://api.deepseek.com/v1',    'openai', 'DEEPSEEK_API_KEY', 'deepseek-v4-pro', 110),
  ('groq',     'https://api.groq.com/openai/v1', 'openai', 'GROQ_API_KEY',     'groq/compound',        120),
  ('mistral',  'https://api.mistral.ai/v1',      'openai', 'MISTRAL_API_KEY',  'mistral-large-latest', 130),
  ('cerebras', 'https://api.cerebras.ai/v1',     'openai', 'CEREBRAS_API_KEY', 'gemma-4-31b',          140);

-- A providers row alone is not routable: chat routing walks agent lists over
-- `models`, so a catalog provider needs a model row for request_config's
-- agent.models to have something to point at. One default model each.
INSERT OR IGNORE INTO models(id, provider_name, model) VALUES
  ('openai/gpt-5.4',              'openai',   'gpt-5.4'),
  ('deepseek/deepseek-v4-pro',    'deepseek', 'deepseek-v4-pro'),
  ('groq/groq/compound',          'groq',     'groq/compound'),
  ('mistral/mistral-large-latest','mistral',  'mistral-large-latest'),
  ('cerebras/gemma-4-31b',        'cerebras', 'gemma-4-31b');

-- ═══ Built-in tools ═══
-- Descriptions are a write-once seed: tools_sync_to_db() upserts with
-- COALESCE(tools.description, excluded.description), so on a fresh DB these
-- rows win and the C-registered description is never consulted again. Keep
-- them in sync with the tools_register() call sites in src/tool_*.c.
INSERT OR IGNORE INTO tools(name, description) VALUES
  ('file_read', 'Read a file (path relative or absolute)'),
  ('file_write', 'Write content to a file (path relative or absolute)'),
  ('shell_exec', 'Execute a shell command and return stdout+stderr'),
  ('js_eval', 'Run JavaScript in QuickJS (modern JS; no modules, no top-level await). http_request(url[, {method, body, headers:{Name:Value}, markdownify}]) is synchronous HTTP; returns {status, body} where body is a string ('''' on error, plus {error}); markdownify:true converts an HTML body to markdown. Use .body, e.g. http_request(url).body.match(/re/).'),
  ('web_fetch', 'Fetch a URL via HTTP GET and return content as markdown. Large pages are truncated; page through with offset + max_chars, or read/grep the full copy saved under workspace .tool_results/ (path given in the result).'),
  ('memory_create', 'Create a new memory block (a named container of numbered notes). Args: label, description.'),
  ('memory_add', 'Add a numbered note to a memory block. Args: block (label), text.'),
  ('memory_edit', 'Replace the text of existing notes by number. Args: block, edits (array of {number, text}).'),
  ('memory_delete', 'Delete notes by number (others renumber). Args: block, numbers (array of integers).'),
  ('request_config', 'Request configuration changes — one batched changes document per approval (requires human approval). The document is a patch against what search_config shows: agent.* fields replace; grants.* add; models/provider upsert; grants.remove narrows immediately without approval.'),
  ('search_config', 'Discover your current configuration and what you can request: your sandbox profile, granted tools and hosts, the full list of available tools, pending approvals, and how to request more via request_config. format:json returns the same state as one canonical document.'),
  ('launch_agent', 'Delegate a task to another agent — blocking, or background with the result delivered to you when the child finishes'),
  ('check_session', 'Peek at a sub-agent session state and its final text once idle — a progress peek, not a way to wait'),
  ('secret_create', 'Mint a new random credential, stored encrypted in the DB. Returns only its {{SECRET:name}} placeholder — never the value.'),
  ('create_agent', 'Propose creation of a named agent (requires human approval). Takes an agent definition; the new agent is capped by the creator''s profile and grants.');
