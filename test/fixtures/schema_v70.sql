-- FROZEN v70 fixture (2026-11-24): the schema of a fresh database made by the
-- v0.8.0 release binary (cclaw approvals on an empty file), dumped from
-- sqlite_master in creation order, the generated change_log triggers included.
-- Do NOT resync from templates/schema.sql: a schema change gets a patch in
-- schema_patches[] and this file stays as the patch test's input.
CREATE TABLE config (
  key           TEXT PRIMARY KEY,
  value         TEXT,
  default_value TEXT,
  description   TEXT,
  secret        INTEGER NOT NULL DEFAULT 0,  -- resolves env → secrets table, never value
  required      INTEGER NOT NULL DEFAULT 0   -- channel launch gate: must resolve non-empty
);
CREATE TABLE providers (
  name TEXT PRIMARY KEY,                      -- the key models and secrets refer to it by
  base_url TEXT NOT NULL,                     -- API endpoint requests are sent to
  endpoint_type TEXT NOT NULL DEFAULT 'openai',  -- wire format: openai | gemini | anthropic | responses |
                                                 -- gemini_interactions
  api_key_env TEXT NOT NULL DEFAULT '',       -- name of the key: an environment variable, else a system
                                              -- secret of that name; empty = no key
  default_model TEXT,                         -- fresh-install seed sugar only (seed.sql); not a routing
                                              -- setting
  priority INTEGER NOT NULL DEFAULT 0,        -- order providers are listed in (lowest first); new providers
                                              -- go last
  -- JSON merged (json_patch, RFC 7386) into every outgoing request body for
  -- this provider as the LAST step of payload assembly, so it can override
  -- anything cclaw built. The escape hatch for provider-specific request
  -- options (e.g. OpenRouter's upstream routing params) — config, never C.
  request_extra TEXT
);
CREATE TABLE models (
  id TEXT PRIMARY KEY,                        -- canonical "<provider>/<model>" id that routing lists use
  provider_name TEXT NOT NULL REFERENCES providers(name) ON UPDATE CASCADE,  -- the provider that serves it
  model TEXT NOT NULL,                        -- the model name as the provider spells it on the wire
  context_window INTEGER,                     -- tokens the model can read; NULL = unknown
  max_output_tokens INTEGER,                  -- cap on tokens per reply; NULL = provider default
  capabilities TEXT DEFAULT '[]',             -- JSON array, e.g. ["tools","image","audio"]; image/audio make
                                              -- it eligible for media lists
  -- How this model spells reasoning effort on the wire, as JSON:
  --   {"format": "openrouter|openai|deepseek|gemini-level|gemini-budget",
  --    "levels": {"off":..,"minimal":..,"low":..,"medium":..,"high":..}}
  -- Level values are wire values (strings for effort enums, integers for
  -- budgets); a missing or null level means this model does not support it,
  -- and a request for it clamps to the nearest level that does. NULL column =
  -- use the endpoint's default map (see specs/providers.md).
  effort_map TEXT,
  status TEXT NOT NULL DEFAULT 'healthy',     -- healthy | degraded (failing, sinks in routing until
                                              -- degraded_until) | disabled (operator)
  degraded_until INTEGER,                     -- end of the current degraded cooldown
  total_requests INTEGER DEFAULT 0,           -- requests sent to this model, ever
  total_tokens_in INTEGER DEFAULT 0,          -- prompt tokens, summed
  total_tokens_out INTEGER DEFAULT 0,         -- completion tokens, summed
  total_cost_nano INTEGER DEFAULT 0,          -- cost reported by providers, in billionths of a dollar
  consec_failures INTEGER DEFAULT 0,          -- failed requests in a row; zeroed by a success
  last_success_at INTEGER,                    -- last successful request
  last_error_at INTEGER,                      -- last failed request
  synced_at INTEGER,                          -- no writer today
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE TABLE agent_models (
  agent_name TEXT NOT NULL REFERENCES agents(name) ON UPDATE CASCADE ON DELETE CASCADE,  -- whose list this is
  purpose TEXT NOT NULL DEFAULT 'chat' CHECK (purpose IN ('chat','image','audio','utility')),
  model_id TEXT NOT NULL REFERENCES models(id),  -- the model at this place in the list
  pos INTEGER NOT NULL,                       -- place in the list, 0 = first choice
  -- How hard this agent wants this model to think: off|minimal|low|medium|high.
  -- NULL (the default) sends nothing, which is what every model did before the
  -- knob existed. The level is translated to wire form at build time through
  -- models.effort_map, clamping to the nearest supported level.
  reasoning_effort TEXT CHECK (reasoning_effort IS NULL OR reasoning_effort IN
    ('off','minimal','low','medium','high')),
  -- When this agent's session is trimmed, read off the first enabled chat
  -- entry (the governing one). Over trim_threshold tokens at the end of an
  -- iteration, the session is cut to prune_target (turn_edit.c, the trim).
  -- NULL trim_threshold = 50% of the model's context_window; NULL
  -- prune_target = 50% of the threshold (context.c, context_limits — the only
  -- copy of those defaults). Opting out of trimming is trim_threshold set to
  -- the window. The threshold may not exceed the window (trigger below).
  trim_threshold INTEGER CHECK (trim_threshold IS NULL OR trim_threshold > 0),
  prune_target INTEGER CHECK (prune_target IS NULL OR (prune_target > 0
    AND (trim_threshold IS NULL OR prune_target < trim_threshold))),
  -- How long the provider keeps this model's prompt cache, in seconds, read
  -- off the governing entry like the two above. NULL = unknown: no
  -- CacheExpiring hook fires. At 90% of it after a session's last request
  -- started, an idle session above prune_target fires CacheExpiring
  -- (settle.c). On an Anthropic-format entry it also selects the cache
  -- marker sent (llm_payload.c): 3600 = the 1-hour marker, 300 or NULL = the
  -- plain 5-minute one. There a value is 300 or 3600, and a new chat entry
  -- that gives none gets 3600 (triggers below); NULL is reached only by
  -- clearing it afterwards. For other providers it is the documented figure,
  -- entered by the operator.
  cache_lifetime INTEGER CHECK (cache_lifetime IS NULL OR cache_lifetime > 0),
  -- Whether the model is asked for a readable summary of its reasoning, kept
  -- as the text of the reasoning entry (summary), or not (none). Mapped per
  -- wire format in llm_payload.c (Anthropic thinking.display, Gemini
  -- includeThoughts); an OpenAI-compatible provider is asked nothing either
  -- way and returns what it returns, which none leaves unstored. The replay
  -- blob is captured regardless (db_response.c).
  reasoning_display TEXT NOT NULL DEFAULT 'summary'
    CHECK (reasoning_display IN ('summary','none')),
  PRIMARY KEY (agent_name, purpose, pos)
);
CREATE TRIGGER agent_models_capability_bi
BEFORE INSERT ON agent_models
WHEN NEW.purpose IN ('image','audio') AND NOT EXISTS (
  SELECT 1 FROM models m, json_each(CASE WHEN json_valid(m.capabilities)
                                         THEN m.capabilities ELSE '[]' END) c
   WHERE m.id = NEW.model_id AND c.value = NEW.purpose)
BEGIN
  SELECT RAISE(ABORT, 'agent_models: the model''s capabilities do not include this purpose');
END;
CREATE TRIGGER agent_models_capability_bu
BEFORE UPDATE OF purpose, model_id ON agent_models
WHEN NEW.purpose IN ('image','audio') AND NOT EXISTS (
  SELECT 1 FROM models m, json_each(CASE WHEN json_valid(m.capabilities)
                                         THEN m.capabilities ELSE '[]' END) c
   WHERE m.id = NEW.model_id AND c.value = NEW.purpose)
BEGIN
  SELECT RAISE(ABORT, 'agent_models: the model''s capabilities do not include this purpose');
END;
CREATE TRIGGER agent_models_window_bi
BEFORE INSERT ON agent_models
WHEN NEW.trim_threshold > (SELECT m.context_window FROM models m WHERE m.id = NEW.model_id)
BEGIN
  SELECT RAISE(ABORT, 'agent_models: trim_threshold is larger than the model''s context window');
END;
CREATE TRIGGER agent_models_window_bu
BEFORE UPDATE OF trim_threshold, model_id ON agent_models
WHEN NEW.trim_threshold > (SELECT m.context_window FROM models m WHERE m.id = NEW.model_id)
BEGIN
  SELECT RAISE(ABORT, 'agent_models: trim_threshold is larger than the model''s context window');
END;
CREATE TRIGGER agent_models_lifetime_bi
BEFORE INSERT ON agent_models
WHEN NEW.cache_lifetime NOT IN (300, 3600) AND EXISTS (
  SELECT 1 FROM models m JOIN providers p ON p.name = m.provider_name
   WHERE m.id = NEW.model_id AND p.endpoint_type = 'anthropic')
BEGIN
  SELECT RAISE(ABORT, 'agent_models: cache_lifetime on an Anthropic model is 300 or 3600');
END;
CREATE TRIGGER agent_models_lifetime_bu
BEFORE UPDATE OF cache_lifetime, model_id ON agent_models
WHEN NEW.cache_lifetime NOT IN (300, 3600) AND EXISTS (
  SELECT 1 FROM models m JOIN providers p ON p.name = m.provider_name
   WHERE m.id = NEW.model_id AND p.endpoint_type = 'anthropic')
BEGIN
  SELECT RAISE(ABORT, 'agent_models: cache_lifetime on an Anthropic model is 300 or 3600');
END;
CREATE TRIGGER agent_models_lifetime_ai
AFTER INSERT ON agent_models
WHEN NEW.cache_lifetime IS NULL AND NEW.purpose = 'chat' AND EXISTS (
  SELECT 1 FROM models m JOIN providers p ON p.name = m.provider_name
   WHERE m.id = NEW.model_id AND p.endpoint_type = 'anthropic')
BEGIN
  UPDATE agent_models SET cache_lifetime = 3600 WHERE rowid = NEW.rowid;
END;
CREATE TABLE models_cache (
  provider_name TEXT NOT NULL,
  id TEXT NOT NULL,
  context_length INTEGER,
  prompt_price TEXT,
  completion_price TEXT,
  modality TEXT,
  synced_at INTEGER NOT NULL,
  PRIMARY KEY (provider_name, id)
);
CREATE TABLE llm_jobs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL,
  agent_name TEXT NOT NULL REFERENCES agents(name) ON UPDATE CASCADE,
  recall INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE INDEX idx_llm_jobs_session ON llm_jobs(session_id);
CREATE TABLE extensions (
  name TEXT PRIMARY KEY,                      -- the key tools, hooks and channels refer to it by
  path TEXT NOT NULL,                       -- shared store dir: ~/.cclaw/extensions/<name>
  version TEXT DEFAULT '0.0.0',               -- the manifest's version
  owner_agent TEXT REFERENCES agents(name) ON UPDATE CASCADE,
                                            -- the agent that promoted it; NULL for an
                                            -- extension shipped in the binary
  published INTEGER NOT NULL DEFAULT 0,     -- the single publish flag
  enabled INTEGER NOT NULL DEFAULT 1,         -- 0 = installed but switched off
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE TABLE agents (
  name TEXT PRIMARY KEY,                      -- the key every agent_name column refers to
  system_prompt TEXT,                         -- prompt template; NULL = the default prompt (tools, memory,
                                              -- skills are added after it)
  description TEXT,                           -- one line saying what the agent is for
  max_iterations INTEGER DEFAULT 25,          -- LLM requests allowed per turn
  max_output_tokens INTEGER,                  -- cap on tokens per reply; NULL = the model's
  shell_timeout INTEGER DEFAULT 30,           -- seconds a shell_exec call may run
  shell_path TEXT,               -- interpreter for shell_exec's -c; NULL = /bin/sh
  sandbox_profile TEXT DEFAULT 'standard',    -- containment: host | standard | restricted (specs/sandbox-
                                              -- profiles.md)
  created_by TEXT REFERENCES agents(name) ON UPDATE CASCADE,
                                 -- creating agent (update_agent authorization); NULL = operator
  hold_until INTEGER,            -- quiesce lease (cclaw rename-agent): while in the
                                 -- future no NEW turn opens for this agent. NULL = no
                                 -- hold. Expiry self-heals — a crashed holder unblocks.
  hold_holder TEXT,              -- who holds the lease ("cli:<pid>"), for the error text
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE TABLE grants (
  agent_name TEXT NOT NULL REFERENCES agents(name) ON UPDATE CASCADE,  -- who holds the grant
  kind TEXT NOT NULL,                         -- tool | host | read_path | write_path | secret
  value TEXT NOT NULL,                        -- the tool name, host, path or secret name granted
  approval_mode TEXT NOT NULL DEFAULT 'silent',  -- tool grants only: silent | always | tool_decides
  expires_at INTEGER,                         -- when the grant stops counting; NULL = never
  created_at INTEGER DEFAULT (unixepoch()),
  source TEXT NOT NULL CHECK (source IN ('approval','operator','agent_create',
    'extension','seed','capture','migration','legacy')),
  approval_id INTEGER REFERENCES approvals(id),
  PRIMARY KEY (agent_name, kind, value)
);
CREATE TABLE agent_extensions (
  agent_name TEXT REFERENCES agents(name) ON UPDATE CASCADE,  -- the agent the extension is attached to
  extension_name TEXT NOT NULL REFERENCES extensions(name) ON UPDATE CASCADE,  -- the attached extension
  config TEXT,                                -- per-agent extension settings (JSON); NULL = none
  enabled INTEGER NOT NULL DEFAULT 1,         -- 0 = attached but switched off for this agent
  PRIMARY KEY (agent_name, extension_name)
);
CREATE TABLE channels (
  name TEXT PRIMARY KEY,                      -- the key chats, sessions and the outbox refer to it by
  extension_name TEXT REFERENCES extensions(name) ON UPDATE CASCADE,
                                            -- the code fronting this channel; NULL = none
  prev_extension_name TEXT REFERENCES extensions(name) ON UPDATE CASCADE,
                                            -- revert target recorded by channel swap
  type TEXT NOT NULL DEFAULT '',            -- manifest $.channel.type (display only)
  status TEXT NOT NULL DEFAULT 'draft',
  pid INTEGER,                                -- process id of the running channel runner; NULL = not running
  default_agent TEXT REFERENCES agents(name) ON UPDATE CASCADE,
                    -- Channel open-door policy: non-NULL = unknown chats are
                    -- accepted and get a new session bound to this agent;
                    -- NULL = fail-closed (unrouted chats drop + admin notify).
                    -- Grants no send authority — channel_send needs a route.
  default_tool_filter TEXT,
                    -- JSON array of tool names; NULL = unrestricted. The starting
                    -- channel_chats.tool_filter of every chat the gate records
                    -- (copied once, when the chat's row is created). Editing it
                    -- changes future chats, never existing ones.
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE TABLE channel_state (
  channel_name TEXT NOT NULL REFERENCES channels(name) ON UPDATE CASCADE,
  key TEXT NOT NULL,
  value TEXT,
  PRIMARY KEY (channel_name, key)
);
CREATE TABLE channel_chats (
  channel_name TEXT NOT NULL REFERENCES channels(name) ON UPDATE CASCADE,
  chat_id TEXT NOT NULL,
  session_id INTEGER REFERENCES sessions(id),  -- the pin; NULL = not routed (never assigned,
                                               -- or the operator removed the route)
  delivery_mode TEXT NOT NULL DEFAULT 'auto'   -- how a turn's output reaches this chat
    CHECK (delivery_mode IN ('auto','iteration','digest','turn','quiescent','explicit')),
                                               -- (specs/delivery.md): 'auto' (= turn) —
                                               -- the final message posts; 'explicit' — nothing
                                               -- posts unless the agent calls channel_send. A new
                                               -- chat starts from its envelope's chat_type (group
                                               -- 'explicit', dm 'auto'); one named before it ever
                                               -- spoke, by its id ('-' prefix: 'explicit'). Set
                                               -- once: a later message never overwrites it.
  tool_filter TEXT,                            -- JSON array of tool names; NULL = unrestricted.
                                               -- Narrows the pinned session's tools every turn
                                               -- (grants ∩ sessions.tool_filter ∩ this). A chat
                                               -- the gate records starts from
                                               -- channels.default_tool_filter.
  guidance TEXT,                               -- appended to the system prompt of this chat's
                                               -- sessions every turn; NULL = nothing appended
  title TEXT,                                  -- observed human-readable name ("#general @ My
                                               -- Server", "DM with alice"); display only — sends
                                               -- always address by chat_id
  is_admin INTEGER,                            -- 1 = the chat's only reader is an operator: a DM
                                               -- whose other party is one of the channel's
                                               -- admin_ids. 0 = anyone else. Set from each
                                               -- message's envelope; NULL until one arrives. A
                                               -- failed turn tells a non-admin chat only a
                                               -- generic line (specs/error-handling.md).
  error_notice_at INTEGER,                     -- when this chat last got that generic line
  kind TEXT CHECK (kind IN ('dm','group')),    -- the envelope's chat_type; NULL = not reported
  first_seen_at INTEGER NOT NULL DEFAULT (unixepoch()),
  last_message_at INTEGER,                     -- the last inbound message, routed or dropped
  notified_at INTEGER,                         -- when the operator was told this unrouted chat
                                               -- spoke; the notice is sent once per chat
  PRIMARY KEY (channel_name, chat_id)
);
CREATE UNIQUE INDEX idx_channel_chats_session
  ON channel_chats(session_id) WHERE session_id IS NOT NULL;
CREATE TABLE sessions (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT,                                  -- a human label for the conversation
  agent_name TEXT REFERENCES agents(name) ON UPDATE CASCADE,  -- the agent that runs this session (never
                                                              -- changes)
  channel_name TEXT,                        -- the channel of the chat this session belongs
                                            -- to; NULL = no chat
  chat_id TEXT,                             -- the chat this session belongs to, with
                                            -- channel_name; NULL = no chat. Older sessions
                                            -- left behind by /new still point at their chat;
                                            -- channel_chats.session_id says which session the
                                            -- chat is pinned to now.
  parent_session_id INTEGER DEFAULT -1,       -- the session that launched this one; -1 = none
  parent_tool_call_id TEXT,                   -- the parent's launch_agent call this session answers
  depth INTEGER NOT NULL DEFAULT 0,           -- how many launches deep: 0 = top level
  tool_filter TEXT,                         -- JSON array of tool names; NULL = unrestricted.
                                            -- Positive scope frozen at spawn (launch_agent),
                                            -- intersected with grants per turn — never grants
                                            -- anything itself. A chat's filter is not copied
                                            -- here: channel_chats.tool_filter is read live.
  state TEXT NOT NULL DEFAULT 'idle',         -- idle | llm_running | tool_running | awaiting_approval |
                                              -- rate_limited | settling (a hook is awaited — at the
                                              -- end of an iteration, or on an idle session whose
                                              -- cache is lapsing; the inbox queues)
  settle_event TEXT,                          -- v68. while settling: the event the hold began
                                              -- with ('Stop', 'SubagentStop', 'ContextThreshold',
                                              -- 'CacheExpiring'); whoever ends the hold reads it —
                                              -- no C check follows a CacheExpiring hold. NULL in
                                              -- every other state
  owner_instance TEXT,                      -- live owner (processes.instance_id); NULL ⟺ state='idle'
  turn_iteration INTEGER NOT NULL DEFAULT 0,  -- LLM requests made so far in the current turn
  turn_context TEXT,                        -- <RELEVANT_CONTEXT> block, materialized once at turn
                                            -- start (llm_proc.c) and reused verbatim by every
                                            -- tool-loop iteration so the request prefix stays
                                            -- byte-stable for prompt caching. NULL = no block.
  turn_context_tokens INTEGER,              -- the provider's count for the block the newest
                                            -- measured request carried (usage_attribute); the
                                            -- next turn's first report subtracts it. NULL = none yet.
  leaf_id INTEGER DEFAULT -1,                 -- the newest entry of the active branch; -1 = none
  cache_armed_at INTEGER,                   -- CacheExpiring is armed: when the session's last
                                            -- request started (a response came back and the
                                            -- governing entry has a cache_lifetime). Cleared
                                            -- when it fires or the cache has lapsed, so it
                                            -- fires once per idle period (settle.c). NULL =
                                            -- not armed.
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  updated_at INTEGER NOT NULL DEFAULT (unixepoch()),
  FOREIGN KEY (channel_name, chat_id) REFERENCES channel_chats(channel_name, chat_id)
    ON UPDATE CASCADE
);
CREATE INDEX idx_sessions_owner ON sessions(owner_instance) WHERE owner_instance IS NOT NULL;
CREATE INDEX idx_sessions_cache_armed ON sessions(cache_armed_at)
  WHERE cache_armed_at IS NOT NULL;
CREATE INDEX idx_sessions_parent ON sessions(parent_session_id)
  WHERE parent_session_id IS NOT NULL;
CREATE TABLE delivery_edges (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL REFERENCES sessions(id),
  target_kind TEXT NOT NULL,             -- 'parent' | 'channel' | 'session' | 'tool_call'
  target_ref TEXT NOT NULL,              -- parent: parent session id; channel: channel name
                                         -- (chat_id lives on the session); tool_call: call_id
  policy TEXT CHECK ((target_kind = 'channel') = (policy IS NULL)),
                                         -- NULL exactly on channel edges (read from the chat)
  cursor INTEGER NOT NULL DEFAULT 0,
  one_shot INTEGER NOT NULL DEFAULT 0,   -- blocking reply edge: fires once, then deleted
  UNIQUE(session_id, target_kind, target_ref)
);
CREATE TABLE entries (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL,                -- the session this entry belongs to
  parent_id INTEGER NOT NULL DEFAULT -1,      -- the entry before this one on its branch; -1 = first
  original_parent_id INTEGER,                 -- the parent this entry was written under, set the
                                              -- first time compaction or a turn edit re-points it
  turn_id INTEGER,          -- the turn this entry belongs to. Minted when a user entry
                            -- follows a non-user entry (the inbox drain's first message,
                            -- or a directly appended user entry); every entry after it
                            -- inherits it from its parent until the next turn opens.
                            -- Filled by entries_turn_ai — see the trigger.
  iteration_id INTEGER,     -- one LLM request/response *inside* a turn: minted per request
                            -- by db_next_iteration_id, shared by the assistant message,
                            -- its tool_call parts and their tool results.
  type TEXT NOT NULL DEFAULT 'user_message',  -- user_message | assistant_message | tool_call | tool_result |
                                              -- reasoning | compaction | cron_result | system | ...
  part_index INTEGER NOT NULL DEFAULT 0,
  role INTEGER NOT NULL DEFAULT 1,            -- who speaks: 0 system, 1 user, 2 assistant, 3 tool, 4
                                              -- compaction
  content TEXT,                               -- the text (for a tool call: its JSON arguments)
  tool_calls TEXT,
  tool_call_id TEXT,                          -- pairs a tool call with its result
  tool_name TEXT,                             -- the tool that was called or answered
  is_error INTEGER NOT NULL DEFAULT 0,        -- 1 = a failed tool result or error message
  stop_reason INTEGER NOT NULL DEFAULT 0,     -- why the model stopped: 1 stop, 2 length, 3 tool use, 4 error,
                                              -- 5 aborted
  model TEXT,                                 -- the model that wrote an assistant entry
  usage_in INTEGER,                           -- prompt tokens of the request that produced it
  usage_out INTEGER,                          -- completion tokens
  cached_tokens INTEGER,   -- subset of usage_in the provider served from cache
                           -- (NULL when the provider doesn't report it)
  cost_nano INTEGER,                          -- cost in billionths of a dollar
  token_estimate INTEGER,                     -- content_bytes*2/5 + 4 (2.5 bytes per token, TOKEN_ESTIMATE
                                              -- in types.h): the size until the provider measures it
  tokens INTEGER,           -- the provider's count for this entry, written once when a
                            -- response reports it (db_response.c, usage_attribute): its
                            -- share of usage_out for what the model wrote, its share of
                            -- the growth in usage_in for what was appended. NULL = not
                            -- measured, read as token_estimate. A reasoning entry holds
                            -- what its replay costs: 0 once it is no longer counted.
  request_cut INTEGER,      -- on a reported assistant_message: 1 when the request that
                            -- produced it had been cut (its oldest turns left out to fit
                            -- the budget, context_plan), so its usage_in does not measure
                            -- the head. NULL = sent whole. Stamped by llm_req.
  content_bytes INTEGER,
  tool_call_count INTEGER NOT NULL DEFAULT 0,
  data TEXT,
  network_hosts TEXT,  -- NULL or JSON array of hosts the tool run contacted
  fence_id TEXT,       -- random id minted with network_hosts; both tags of the
                       -- untrusted-content wrap carry it (llm_payload.c)
  -- Reasoning replay blob, set on type='reasoning' entries only:
  -- {provider, model, format, blob}. provider/model tag who produced it
  -- (replay strips on model switch); format is the wire shape —
  -- 'reasoning_details' (OpenRouter array, replayed verbatim: this is how
  -- Gemini thoughtSignature round-trips on the OpenAI-compat path),
  -- 'reasoning_content' (DeepSeek-style bare string), or 'gemini_parts'
  -- ([{fn,sig}] — native thoughtSignature, per functionCall). Replay always
  -- reads $.blob, never entries.content: the routing entry's
  -- reasoning_display governs the display text only, while the wire
  -- requirement holds either way.
  reasoning_meta TEXT,
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE INDEX idx_entries_session ON entries(session_id, id);
CREATE INDEX idx_entries_iteration_type ON entries(session_id, iteration_id, type, part_index);
CREATE INDEX idx_entries_created ON entries(created_at);
CREATE VIRTUAL TABLE entries_fts USING fts5(
  content, content=entries, content_rowid=id,
  tokenize='porter unicode61'
);
CREATE TRIGGER entries_ai AFTER INSERT ON entries BEGIN
  INSERT INTO entries_fts(rowid, content) VALUES (
    new.id,
    CASE new.type
      WHEN 'tool_call' THEN COALESCE(new.tool_name,'') || ' ' || COALESCE(new.content,'')
      WHEN 'tool_result' THEN COALESCE(new.tool_name,'') || ' ' || COALESCE(new.content,'')
      ELSE COALESCE(new.content,'')
    END
  );
END;
CREATE TRIGGER entries_leaf_ai AFTER INSERT ON entries
WHEN NEW.type != 'compaction'
BEGIN
  UPDATE sessions SET leaf_id = NEW.id, updated_at = unixepoch() WHERE id = NEW.session_id;
END;
CREATE TRIGGER entries_turn_ai AFTER INSERT ON entries
WHEN NEW.turn_id IS NULL
BEGIN
  UPDATE entries SET turn_id = COALESCE(
      (SELECT p.turn_id FROM entries p
        WHERE p.id = NEW.parent_id AND NOT (NEW.role = 1 AND p.role <> 1)),
      (SELECT COALESCE(MAX(turn_id), 0) + 1 FROM entries WHERE session_id = NEW.session_id))
    WHERE id = NEW.id;
END;
CREATE TABLE tool_calls (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL,
  entry_id INTEGER NOT NULL,
  call_id TEXT NOT NULL,
  name TEXT NOT NULL,
  result_entry_id INTEGER,
  status TEXT NOT NULL DEFAULT 'pending',
  resolved_by TEXT,
  resolved_at INTEGER
);
CREATE INDEX idx_tool_calls_entry ON tool_calls(entry_id);
CREATE INDEX idx_tool_calls_session ON tool_calls(session_id, status);
CREATE TABLE llm_responses (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL,
  iteration_id INTEGER NOT NULL, -- the entries.iteration_id this request belongs to
  model TEXT,
  status TEXT NOT NULL,          -- ok | empty | malformed | http_<code> | timeout | network_error
  provider_id TEXT,              -- provider's own response id ($.id), NULL if absent
  upstream_provider TEXT,        -- upstream backend name ($.provider, e.g. OpenRouter's 'StreamLake')
  body BLOB,                     -- JSONB when parseable, raw text otherwise
  request_body BLOB,             -- the payload we sent, every archived row (subject to llm_response_archive_max pruning)
  -- Usage parity (M4): richer usage fields when the provider reports them,
  -- NULL otherwise. cached_tokens/cache_write_tokens are SUBSETS of the
  -- prompt token count on every provider we support today (cache-inclusive
  -- wire semantics); a cache-exclusive provider would need normalizing here.
  cached_tokens INTEGER,
  cache_write_tokens INTEGER,
  reasoning_tokens INTEGER,      -- subset of completion tokens
  cost REAL,                     -- provider-reported cost in dollars
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE INDEX idx_llm_responses_iteration ON llm_responses(session_id, iteration_id);
CREATE TABLE channel_events (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  channel_name TEXT NOT NULL,
  event_type TEXT NOT NULL DEFAULT 'message',
  payload TEXT NOT NULL,
  external_id TEXT,
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  UNIQUE(channel_name, external_id)
);
CREATE TABLE channel_outbox (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  channel_name TEXT NOT NULL,
  session_id INTEGER NOT NULL,
  payload TEXT NOT NULL,
  status TEXT NOT NULL DEFAULT 'pending',
  attempts INTEGER NOT NULL DEFAULT 0,
  next_attempt_at INTEGER NOT NULL DEFAULT 0,
  -- Delivery degradation ladder: 0=formatted (platform rich text), 1=plain.
  -- On a terminal 4xx for a formatted row the C loop re-delivers once at
  -- plain — platform-agnostic: it never inspects WHY the format was
  -- rejected. The ladder only descends, so re-delivery can't loop.
  deliver_mode INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  acked_at INTEGER
);
CREATE INDEX idx_channel_outbox_pending ON channel_outbox(channel_name, status) WHERE status='pending';
CREATE TABLE media_jobs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL,
  source TEXT NOT NULL DEFAULT '',
  payload TEXT NOT NULL,
  attempts INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE TABLE inbox (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL,
  source TEXT NOT NULL DEFAULT 'cli',
  source_ref TEXT,                           -- provenance whose semantics belong to source:
                                             -- 'cron' → job name, 'agent_result' → child
                                             -- session id. NULL = no reference. Drained as a
                                             -- [tag] prefix so it survives without a join.
  payload TEXT NOT NULL,
  consumed INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE INDEX idx_inbox_pending ON inbox(session_id, consumed) WHERE consumed = 0;
CREATE TABLE tools (
  name TEXT PRIMARY KEY,                      -- the name the model calls it by
  extension_name TEXT REFERENCES extensions(name) ON UPDATE CASCADE,
                                             -- NULL for built-in C tools
  description TEXT,                           -- what the model is told the tool does
  parameters_json TEXT,                       -- JSON schema of its arguments
  path TEXT,                                 -- handler file (in the shared store)
  agent_name TEXT REFERENCES agents(name) ON UPDATE CASCADE,
                                             -- owner scope, NULL = global
  enabled INTEGER NOT NULL DEFAULT 1,         -- 0 = registered but switched off
  policy TEXT,                                -- JSON per-argument rules (allow / ask / deny); NULL = none
  egress_hosts TEXT,                         -- comma-separated hosts declared by the
                                             -- manifest ($.tools[].hosts). Non-NULL
                                             -- REPLACES the agent's host grants for
                                             -- calls of this tool (Q1); NULL = the tool
                                             -- runs under the agent's grants. Only a
                                             -- promoted extension tool can carry one.
  droppable INTEGER NOT NULL DEFAULT 0        -- 1 = a hook may take a call of this
                                             -- tool out of context. 0 = the call exists for
                                             -- its effect and always stays (it can still be
                                             -- trimmed). Built-in: the C recipe's flag.
                                             -- Extension: $.tools[].droppable, protected
                                             -- when the manifest does not say.
);
CREATE TABLE hooks (
  extension_name TEXT NOT NULL REFERENCES extensions(name) ON UPDATE CASCADE,
  event TEXT NOT NULL,                        -- event name: 'Stop', 'SubagentStop',
                                              -- 'ContextThreshold', 'CacheExpiring'
  path TEXT NOT NULL,                         -- handler file (in the shared store)
  enabled INTEGER NOT NULL DEFAULT 1,
  async INTEGER NOT NULL DEFAULT 0,           -- v68. 0 = the session is held until the script ends
                                              -- and its reply is applied; 1 = the manifest's
                                              -- "async": started and not awaited, output discarded
  fail_count INTEGER NOT NULL DEFAULT 0,      -- consecutive failed runs, any session; zeroed by a
                                              -- run that works
  input TEXT,                                 -- JSON: what the script is sent besides the newest
                                              -- turn — the manifest's $.hooks[].input
                                              -- ({history, history_results, max_bytes}), validated
                                              -- at promote. NULL = nothing declared: the turn alone,
                                              -- at most 1 MB (specs/turn-edit.md#input)
  pos INTEGER NOT NULL DEFAULT 0,             -- v68. its index in the manifest's hooks[]: an
                                              -- extension's hooks for one event run in this order
  timeout INTEGER                             -- v68. seconds the script may run, at most
    CHECK (timeout IS NULL OR timeout BETWEEN 1 AND 3600), -- HOOK_TIMEOUT_MAX; NULL = config
                                              -- hook_timeout
  disabled INTEGER NOT NULL DEFAULT 0,        -- v68. 1 = turned off after 3 failures in a row
                                              -- (crash, timeout, refused reply); the default
                                              -- agent was told once. Set back to 0, or promote
                                              -- the extension again, to re-enable it
  PRIMARY KEY (extension_name, event, path)
);
CREATE TABLE memory_blocks (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  created_by TEXT REFERENCES agents(name) ON UPDATE CASCADE,
  label TEXT NOT NULL,                        -- the name agents address the block by
  description TEXT,                           -- what the block is for, shown to agents
  char_limit INTEGER NOT NULL DEFAULT 5000,   -- most characters the notes may hold, for agents (the operator
                                              -- is not bound)
  read_only INTEGER NOT NULL DEFAULT 0,   -- 1 = no agent may change it
  placement TEXT NOT NULL DEFAULT 'system',   -- 'system' (system prompt) or 'context' (each turn's context
                                              -- block)
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  updated_at INTEGER NOT NULL DEFAULT (unixepoch()),  -- last change to the block or its notes
  UNIQUE(created_by, label)
);
CREATE TABLE agent_memory_blocks (
  agent_name TEXT NOT NULL REFERENCES agents(name)
             ON UPDATE CASCADE ON DELETE CASCADE,
  block_id   INTEGER NOT NULL REFERENCES memory_blocks(id) ON DELETE CASCADE,
  read_only  INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (agent_name, block_id)
);
CREATE TABLE memory_notes (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  block_id INTEGER NOT NULL REFERENCES memory_blocks(id) ON DELETE CASCADE,  -- the block the note is in
  text TEXT NOT NULL,                         -- the note
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  updated_at INTEGER NOT NULL DEFAULT (unixepoch())  -- last time the text changed
);
CREATE INDEX idx_memory_notes_block
  ON memory_notes(block_id, id);
CREATE TABLE cron_jobs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  agent_name TEXT REFERENCES agents(name) ON UPDATE CASCADE,  -- the agent the job belongs to
  name TEXT NOT NULL,                         -- the job name, unique per agent
  cron_expr TEXT NOT NULL,                    -- 5-field schedule; '' when run_at or interval_s is used
  run_at INTEGER,                            -- one-shot: fire once at this time
  interval_s INTEGER,                        -- fixed period: refire every N seconds
  session_id INTEGER NOT NULL,                -- the session the job was set from
  task TEXT NOT NULL,                         -- the message the agent gets when it fires
  script TEXT,                               -- workspace-relative QJS path; runs sandboxed at
                                             -- fire time. NULL = no script payload
  channel_name TEXT,                         -- chat stamp, with chat_id: the durable identity
                                             -- a fire routes through; NULL = no chat
  chat_id TEXT,                              -- chat stamp, with channel_name: a fire goes to
                                             -- the session this chat pins
                                             -- (channel_chats.session_id, resolved at fire
                                             -- time); NULL = no chat
  target TEXT,                               -- NULL = follow the conversation (stamped chat,
                                             -- else stamped session_id); 'pin' = explicit
                                             -- session pin; 'new' = fresh session per fire
  target_agent TEXT REFERENCES agents(name) ON UPDATE CASCADE,
                                             -- agent for 'new'-mode fires; NULL = agent_name
  enabled INTEGER NOT NULL DEFAULT 1,         -- 0 = paused
  next_run_at INTEGER NOT NULL DEFAULT 0,     -- when it fires next
  last_run_at INTEGER,                        -- when it last fired
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  FOREIGN KEY (channel_name, chat_id) REFERENCES channel_chats(channel_name, chat_id)
    ON UPDATE CASCADE
);
CREATE UNIQUE INDEX idx_cron_jobs_name ON cron_jobs(agent_name, name);
CREATE TABLE approvals (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id   INTEGER NOT NULL,              -- the session that asked
  tool_call_id TEXT,                          -- the tool call that parked
  tool_name    TEXT,                         -- WHAT parked: the tool that asked
  park_reason  TEXT,                         -- WHY it parked: 'approval_required' (the
                                             -- gate/tool asked) or 'sensitive_target'
                                             -- (specs/trust.md rule 1 — per-call, never
                                             -- satisfiable by a standing grant)
  args_json    TEXT,                          -- the call as asked ({{SECRET:name}} placeholders, never
                                              -- values)
  resolve      TEXT NOT NULL DEFAULT 'rerun',  -- 'rerun' (run the call on approval) or 'apply' (apply the
                                               -- request document)
  state        TEXT NOT NULL DEFAULT 'pending',  -- pending | approved | consumed | denied | expired
  decided_via  TEXT,                          -- who or what decided (dashboard, a channel, auto:...)
  requested_at INTEGER DEFAULT (unixepoch()),  -- when it was asked
  expires_at   INTEGER
);
CREATE INDEX approvals_pending ON approvals(session_id, state) WHERE state='pending';
CREATE TABLE approval_decisions (
  approval_id INTEGER PRIMARY KEY REFERENCES approvals(id) ON UPDATE CASCADE,
  decision    TEXT NOT NULL CHECK (decision IN ('deny','once','always')),
  decided_via TEXT NOT NULL,
  created_at  INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE TABLE shares (
  token      TEXT PRIMARY KEY,
  agent_name TEXT NOT NULL REFERENCES agents(name) ON UPDATE CASCADE ON DELETE CASCADE,
  path       TEXT NOT NULL,
  name       TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  expires_at INTEGER NOT NULL
);
CREATE TABLE sensitive_targets (
  kind  TEXT NOT NULL DEFAULT 'host',        -- only 'host' in v1
  value TEXT NOT NULL,                       -- exact host or '.suffix' rule
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  PRIMARY KEY (kind, value)
);
CREATE TABLE secret_hosts (
  secret_name TEXT NOT NULL,
  host        TEXT NOT NULL,                 -- exact host or '.suffix' rule
  created_at  INTEGER NOT NULL DEFAULT (unixepoch()),
  PRIMARY KEY (secret_name, host)
);
CREATE TABLE secrets (
  name       TEXT PRIMARY KEY,                    -- ^[A-Z][A-Z0-9_]*$
  value      TEXT NOT NULL,                   -- encrypted; never shown, logged or sent to a model
  source     TEXT NOT NULL DEFAULT 'operator',      -- 'operator'|'generated'|'captured'
  scope      TEXT NOT NULL DEFAULT 'agent'    -- 'agent' (usable in tool calls when granted) or 'system'
                                              -- (provider keys, daemon only)
             CHECK (scope IN ('agent','system')),
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE TABLE processes (
  instance_id  TEXT PRIMARY KEY,
  pid          INTEGER NOT NULL,
  mode         TEXT NOT NULL,                 -- 'daemon' | 'cli'
  started_at   INTEGER NOT NULL DEFAULT (unixepoch()),
  heartbeat_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE TABLE change_log (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  at          INTEGER NOT NULL DEFAULT (unixepoch()),
  actor       TEXT NOT NULL,
  context     TEXT,
  table_name  TEXT NOT NULL,
  row_key     TEXT NOT NULL,
  action      TEXT NOT NULL CHECK (action IN ('insert','update','delete')),
  old_values  TEXT,                          -- JSON of settings; NULL on insert
  new_values  TEXT                           -- JSON of settings; NULL on delete
);
CREATE TRIGGER change_log_append_only_upd
BEFORE UPDATE ON change_log
BEGIN
  SELECT RAISE(ABORT, 'change_log is append-only');
END;
CREATE TRIGGER change_log_append_only_del
BEFORE DELETE ON change_log
BEGIN
  SELECT RAISE(ABORT, 'change_log is append-only');
END;
CREATE TRIGGER log_agents_ins AFTER INSERT ON "agents" BEGIN INSERT INTO change_log(actor, context, table_name, row_key, action, old_values, new_values) VALUES (cclaw_actor(), cclaw_context(), 'agents', json_object('name', NEW."name"), 'insert', NULL, json_object('name', NEW."name", 'system_prompt', NEW."system_prompt", 'description', NEW."description", 'max_iterations', NEW."max_iterations", 'max_output_tokens', NEW."max_output_tokens", 'shell_timeout', NEW."shell_timeout", 'shell_path', NEW."shell_path", 'sandbox_profile', NEW."sandbox_profile", 'created_by', NEW."created_by")); END;
CREATE TRIGGER log_agents_upd AFTER UPDATE OF "name", "system_prompt", "description", "max_iterations", "max_output_tokens", "shell_timeout", "shell_path", "sandbox_profile", "created_by" ON "agents" WHEN OLD."name" IS NOT NEW."name" OR OLD."system_prompt" IS NOT NEW."system_prompt" OR OLD."description" IS NOT NEW."description" OR OLD."max_iterations" IS NOT NEW."max_iterations" OR OLD."max_output_tokens" IS NOT NEW."max_output_tokens" OR OLD."shell_timeout" IS NOT NEW."shell_timeout" OR OLD."shell_path" IS NOT NEW."shell_path" OR OLD."sandbox_profile" IS NOT NEW."sandbox_profile" OR OLD."created_by" IS NOT NEW."created_by" BEGIN INSERT INTO change_log(actor, context, table_name, row_key, action, old_values, new_values) VALUES (cclaw_actor(), cclaw_context(), 'agents', json_object('name', NEW."name"), 'update', json_object('name', OLD."name", 'system_prompt', OLD."system_prompt", 'description', OLD."description", 'max_iterations', OLD."max_iterations", 'max_output_tokens', OLD."max_output_tokens", 'shell_timeout', OLD."shell_timeout", 'shell_path', OLD."shell_path", 'sandbox_profile', OLD."sandbox_profile", 'created_by', OLD."created_by"), json_object('name', NEW."name", 'system_prompt', NEW."system_prompt", 'description', NEW."description", 'max_iterations', NEW."max_iterations", 'max_output_tokens', NEW."max_output_tokens", 'shell_timeout', NEW."shell_timeout", 'shell_path', NEW."shell_path", 'sandbox_profile', NEW."sandbox_profile", 'created_by', NEW."created_by")); END;
CREATE TRIGGER log_agents_del AFTER DELETE ON "agents" BEGIN INSERT INTO change_log(actor, context, table_name, row_key, action, old_values, new_values) VALUES (cclaw_actor(), cclaw_context(), 'agents', json_object('name', OLD."name"), 'delete', json_object('name', OLD."name", 'system_prompt', OLD."system_prompt", 'description', OLD."description", 'max_iterations', OLD."max_iterations", 'max_output_tokens', OLD."max_output_tokens", 'shell_timeout', OLD."shell_timeout", 'shell_path', OLD."shell_path", 'sandbox_profile', OLD."sandbox_profile", 'created_by', OLD."created_by"), NULL); END;
CREATE TRIGGER log_grants_ins AFTER INSERT ON "grants" BEGIN INSERT INTO change_log(actor, context, table_name, row_key, action, old_values, new_values) VALUES (cclaw_actor(), cclaw_context(), 'grants', json_object('agent_name', NEW."agent_name", 'kind', NEW."kind", 'value', NEW."value"), 'insert', NULL, json_object('agent_name', NEW."agent_name", 'kind', NEW."kind", 'value', NEW."value", 'approval_mode', NEW."approval_mode", 'expires_at', NEW."expires_at", 'source', NEW."source", 'approval_id', NEW."approval_id")); END;
CREATE TRIGGER log_grants_upd AFTER UPDATE OF "agent_name", "kind", "value", "approval_mode", "expires_at", "source", "approval_id" ON "grants" WHEN OLD."agent_name" IS NOT NEW."agent_name" OR OLD."kind" IS NOT NEW."kind" OR OLD."value" IS NOT NEW."value" OR OLD."approval_mode" IS NOT NEW."approval_mode" OR OLD."expires_at" IS NOT NEW."expires_at" OR OLD."source" IS NOT NEW."source" OR OLD."approval_id" IS NOT NEW."approval_id" BEGIN INSERT INTO change_log(actor, context, table_name, row_key, action, old_values, new_values) VALUES (cclaw_actor(), cclaw_context(), 'grants', json_object('agent_name', NEW."agent_name", 'kind', NEW."kind", 'value', NEW."value"), 'update', json_object('agent_name', OLD."agent_name", 'kind', OLD."kind", 'value', OLD."value", 'approval_mode', OLD."approval_mode", 'expires_at', OLD."expires_at", 'source', OLD."source", 'approval_id', OLD."approval_id"), json_object('agent_name', NEW."agent_name", 'kind', NEW."kind", 'value', NEW."value", 'approval_mode', NEW."approval_mode", 'expires_at', NEW."expires_at", 'source', NEW."source", 'approval_id', NEW."approval_id")); END;
CREATE TRIGGER log_grants_del AFTER DELETE ON "grants" BEGIN INSERT INTO change_log(actor, context, table_name, row_key, action, old_values, new_values) VALUES (cclaw_actor(), cclaw_context(), 'grants', json_object('agent_name', OLD."agent_name", 'kind', OLD."kind", 'value', OLD."value"), 'delete', json_object('agent_name', OLD."agent_name", 'kind', OLD."kind", 'value', OLD."value", 'approval_mode', OLD."approval_mode", 'expires_at', OLD."expires_at", 'source', OLD."source", 'approval_id', OLD."approval_id"), NULL); END;
CREATE TRIGGER log_agent_models_ins AFTER INSERT ON "agent_models" BEGIN INSERT INTO change_log(actor, context, table_name, row_key, action, old_values, new_values) VALUES (cclaw_actor(), cclaw_context(), 'agent_models', json_object('agent_name', NEW."agent_name", 'purpose', NEW."purpose", 'pos', NEW."pos"), 'insert', NULL, json_object('agent_name', NEW."agent_name", 'purpose', NEW."purpose", 'model_id', NEW."model_id", 'pos', NEW."pos", 'reasoning_effort', NEW."reasoning_effort", 'trim_threshold', NEW."trim_threshold", 'prune_target', NEW."prune_target", 'cache_lifetime', NEW."cache_lifetime", 'reasoning_display', NEW."reasoning_display")); END;
CREATE TRIGGER log_agent_models_upd AFTER UPDATE OF "agent_name", "purpose", "model_id", "pos", "reasoning_effort", "trim_threshold", "prune_target", "cache_lifetime", "reasoning_display" ON "agent_models" WHEN OLD."agent_name" IS NOT NEW."agent_name" OR OLD."purpose" IS NOT NEW."purpose" OR OLD."model_id" IS NOT NEW."model_id" OR OLD."pos" IS NOT NEW."pos" OR OLD."reasoning_effort" IS NOT NEW."reasoning_effort" OR OLD."trim_threshold" IS NOT NEW."trim_threshold" OR OLD."prune_target" IS NOT NEW."prune_target" OR OLD."cache_lifetime" IS NOT NEW."cache_lifetime" OR OLD."reasoning_display" IS NOT NEW."reasoning_display" BEGIN INSERT INTO change_log(actor, context, table_name, row_key, action, old_values, new_values) VALUES (cclaw_actor(), cclaw_context(), 'agent_models', json_object('agent_name', NEW."agent_name", 'purpose', NEW."purpose", 'pos', NEW."pos"), 'update', json_object('agent_name', OLD."agent_name", 'purpose', OLD."purpose", 'model_id', OLD."model_id", 'pos', OLD."pos", 'reasoning_effort', OLD."reasoning_effort", 'trim_threshold', OLD."trim_threshold", 'prune_target', OLD."prune_target", 'cache_lifetime', OLD."cache_lifetime", 'reasoning_display', OLD."reasoning_display"), json_object('agent_name', NEW."agent_name", 'purpose', NEW."purpose", 'model_id', NEW."model_id", 'pos', NEW."pos", 'reasoning_effort', NEW."reasoning_effort", 'trim_threshold', NEW."trim_threshold", 'prune_target', NEW."prune_target", 'cache_lifetime', NEW."cache_lifetime", 'reasoning_display', NEW."reasoning_display")); END;
CREATE TRIGGER log_agent_models_del AFTER DELETE ON "agent_models" BEGIN INSERT INTO change_log(actor, context, table_name, row_key, action, old_values, new_values) VALUES (cclaw_actor(), cclaw_context(), 'agent_models', json_object('agent_name', OLD."agent_name", 'purpose', OLD."purpose", 'pos', OLD."pos"), 'delete', json_object('agent_name', OLD."agent_name", 'purpose', OLD."purpose", 'model_id', OLD."model_id", 'pos', OLD."pos", 'reasoning_effort', OLD."reasoning_effort", 'trim_threshold', OLD."trim_threshold", 'prune_target', OLD."prune_target", 'cache_lifetime', OLD."cache_lifetime", 'reasoning_display', OLD."reasoning_display"), NULL); END;
CREATE TRIGGER log_providers_ins AFTER INSERT ON "providers" BEGIN INSERT INTO change_log(actor, context, table_name, row_key, action, old_values, new_values) VALUES (cclaw_actor(), cclaw_context(), 'providers', json_object('name', NEW."name"), 'insert', NULL, json_object('name', NEW."name", 'base_url', NEW."base_url", 'endpoint_type', NEW."endpoint_type", 'api_key_env', NEW."api_key_env", 'default_model', NEW."default_model", 'priority', NEW."priority", 'request_extra', NEW."request_extra")); END;
CREATE TRIGGER log_providers_upd AFTER UPDATE OF "name", "base_url", "endpoint_type", "api_key_env", "default_model", "priority", "request_extra" ON "providers" WHEN OLD."name" IS NOT NEW."name" OR OLD."base_url" IS NOT NEW."base_url" OR OLD."endpoint_type" IS NOT NEW."endpoint_type" OR OLD."api_key_env" IS NOT NEW."api_key_env" OR OLD."default_model" IS NOT NEW."default_model" OR OLD."priority" IS NOT NEW."priority" OR OLD."request_extra" IS NOT NEW."request_extra" BEGIN INSERT INTO change_log(actor, context, table_name, row_key, action, old_values, new_values) VALUES (cclaw_actor(), cclaw_context(), 'providers', json_object('name', NEW."name"), 'update', json_object('name', OLD."name", 'base_url', OLD."base_url", 'endpoint_type', OLD."endpoint_type", 'api_key_env', OLD."api_key_env", 'default_model', OLD."default_model", 'priority', OLD."priority", 'request_extra', OLD."request_extra"), json_object('name', NEW."name", 'base_url', NEW."base_url", 'endpoint_type', NEW."endpoint_type", 'api_key_env', NEW."api_key_env", 'default_model', NEW."default_model", 'priority', NEW."priority", 'request_extra', NEW."request_extra")); END;
CREATE TRIGGER log_providers_del AFTER DELETE ON "providers" BEGIN INSERT INTO change_log(actor, context, table_name, row_key, action, old_values, new_values) VALUES (cclaw_actor(), cclaw_context(), 'providers', json_object('name', OLD."name"), 'delete', json_object('name', OLD."name", 'base_url', OLD."base_url", 'endpoint_type', OLD."endpoint_type", 'api_key_env', OLD."api_key_env", 'default_model', OLD."default_model", 'priority', OLD."priority", 'request_extra', OLD."request_extra"), NULL); END;
CREATE TRIGGER log_models_ins AFTER INSERT ON "models" BEGIN INSERT INTO change_log(actor, context, table_name, row_key, action, old_values, new_values) VALUES (cclaw_actor(), cclaw_context(), 'models', json_object('id', NEW."id"), 'insert', NULL, json_object('id', NEW."id", 'provider_name', NEW."provider_name", 'model', NEW."model", 'context_window', NEW."context_window", 'max_output_tokens', NEW."max_output_tokens", 'capabilities', NEW."capabilities", 'effort_map', NEW."effort_map", 'status', NEW."status")); END;
CREATE TRIGGER log_models_upd AFTER UPDATE OF "id", "provider_name", "model", "context_window", "max_output_tokens", "capabilities", "effort_map", "status" ON "models" WHEN OLD."id" IS NOT NEW."id" OR OLD."provider_name" IS NOT NEW."provider_name" OR OLD."model" IS NOT NEW."model" OR OLD."context_window" IS NOT NEW."context_window" OR OLD."max_output_tokens" IS NOT NEW."max_output_tokens" OR OLD."capabilities" IS NOT NEW."capabilities" OR OLD."effort_map" IS NOT NEW."effort_map" OR OLD."status" IS NOT NEW."status" BEGIN INSERT INTO change_log(actor, context, table_name, row_key, action, old_values, new_values) VALUES (cclaw_actor(), cclaw_context(), 'models', json_object('id', NEW."id"), 'update', json_object('id', OLD."id", 'provider_name', OLD."provider_name", 'model', OLD."model", 'context_window', OLD."context_window", 'max_output_tokens', OLD."max_output_tokens", 'capabilities', OLD."capabilities", 'effort_map', OLD."effort_map", 'status', OLD."status"), json_object('id', NEW."id", 'provider_name', NEW."provider_name", 'model', NEW."model", 'context_window', NEW."context_window", 'max_output_tokens', NEW."max_output_tokens", 'capabilities', NEW."capabilities", 'effort_map', NEW."effort_map", 'status', NEW."status")); END;
CREATE TRIGGER log_models_del AFTER DELETE ON "models" BEGIN INSERT INTO change_log(actor, context, table_name, row_key, action, old_values, new_values) VALUES (cclaw_actor(), cclaw_context(), 'models', json_object('id', OLD."id"), 'delete', json_object('id', OLD."id", 'provider_name', OLD."provider_name", 'model', OLD."model", 'context_window', OLD."context_window", 'max_output_tokens', OLD."max_output_tokens", 'capabilities', OLD."capabilities", 'effort_map', OLD."effort_map", 'status', OLD."status"), NULL); END;
CREATE TRIGGER log_secrets_ins AFTER INSERT ON "secrets" BEGIN INSERT INTO change_log(actor, context, table_name, row_key, action, old_values, new_values) VALUES (cclaw_actor(), cclaw_context(), 'secrets', json_object('name', NEW."name"), 'insert', NULL, json_object('name', NEW."name", 'source', NEW."source", 'scope', NEW."scope", 'value', '(set)')); END;
CREATE TRIGGER log_secrets_upd AFTER UPDATE OF "name", "value", "source", "scope" ON "secrets" WHEN OLD."name" IS NOT NEW."name" OR OLD."value" IS NOT NEW."value" OR OLD."source" IS NOT NEW."source" OR OLD."scope" IS NOT NEW."scope" BEGIN INSERT INTO change_log(actor, context, table_name, row_key, action, old_values, new_values) VALUES (cclaw_actor(), cclaw_context(), 'secrets', json_object('name', NEW."name"), 'update', json_object('name', OLD."name", 'source', OLD."source", 'scope', OLD."scope"), json_object('name', NEW."name", 'source', NEW."source", 'scope', NEW."scope", 'value', CASE WHEN OLD."value" IS NOT NEW."value" THEN '(changed)' ELSE '(unchanged)' END)); END;
CREATE TRIGGER log_secrets_del AFTER DELETE ON "secrets" BEGIN INSERT INTO change_log(actor, context, table_name, row_key, action, old_values, new_values) VALUES (cclaw_actor(), cclaw_context(), 'secrets', json_object('name', OLD."name"), 'delete', json_object('name', OLD."name", 'source', OLD."source", 'scope', OLD."scope"), NULL); END;
CREATE TRIGGER log_channel_chats_ins AFTER INSERT ON "channel_chats" BEGIN INSERT INTO change_log(actor, context, table_name, row_key, action, old_values, new_values) VALUES (cclaw_actor(), cclaw_context(), 'channel_chats', json_object('channel_name', NEW."channel_name", 'chat_id', NEW."chat_id"), 'insert', NULL, json_object('channel_name', NEW."channel_name", 'chat_id', NEW."chat_id", 'session_id', NEW."session_id", 'delivery_mode', NEW."delivery_mode", 'tool_filter', NEW."tool_filter", 'guidance', NEW."guidance")); END;
CREATE TRIGGER log_channel_chats_upd AFTER UPDATE OF "channel_name", "chat_id", "session_id", "delivery_mode", "tool_filter", "guidance" ON "channel_chats" WHEN OLD."channel_name" IS NOT NEW."channel_name" OR OLD."chat_id" IS NOT NEW."chat_id" OR OLD."session_id" IS NOT NEW."session_id" OR OLD."delivery_mode" IS NOT NEW."delivery_mode" OR OLD."tool_filter" IS NOT NEW."tool_filter" OR OLD."guidance" IS NOT NEW."guidance" BEGIN INSERT INTO change_log(actor, context, table_name, row_key, action, old_values, new_values) VALUES (cclaw_actor(), cclaw_context(), 'channel_chats', json_object('channel_name', NEW."channel_name", 'chat_id', NEW."chat_id"), 'update', json_object('channel_name', OLD."channel_name", 'chat_id', OLD."chat_id", 'session_id', OLD."session_id", 'delivery_mode', OLD."delivery_mode", 'tool_filter', OLD."tool_filter", 'guidance', OLD."guidance"), json_object('channel_name', NEW."channel_name", 'chat_id', NEW."chat_id", 'session_id', NEW."session_id", 'delivery_mode', NEW."delivery_mode", 'tool_filter', NEW."tool_filter", 'guidance', NEW."guidance")); END;
CREATE TRIGGER log_channel_chats_del AFTER DELETE ON "channel_chats" BEGIN INSERT INTO change_log(actor, context, table_name, row_key, action, old_values, new_values) VALUES (cclaw_actor(), cclaw_context(), 'channel_chats', json_object('channel_name', OLD."channel_name", 'chat_id', OLD."chat_id"), 'delete', json_object('channel_name', OLD."channel_name", 'chat_id', OLD."chat_id", 'session_id', OLD."session_id", 'delivery_mode', OLD."delivery_mode", 'tool_filter', OLD."tool_filter", 'guidance', OLD."guidance"), NULL); END;
