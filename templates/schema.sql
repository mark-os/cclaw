-- cclaw.db unified schema
-- Single file, WAL mode. CLI and daemon are peers sharing one source of truth;
-- per-session ownership (sessions.owner_instance → processes) makes recovery
-- owner-scoped so a live peer's in-flight sessions are never stomped.

-- ═══ Global settings ═══
-- Registry-backed (src/config_registry.c): default_value + description are
-- code-owned and resynced every startup; value is the operator/agent override
-- (NULL = use default). Effective value = env CCLAW_<KEY> ?? value ??
-- default_value (specs/config.md).
CREATE TABLE IF NOT EXISTS config (
  key           TEXT PRIMARY KEY,
  value         TEXT,
  default_value TEXT,
  description   TEXT,
  secret        INTEGER NOT NULL DEFAULT 0,  -- resolves env → secrets table, never value
  required      INTEGER NOT NULL DEFAULT 0   -- channel launch gate: must resolve non-empty
);

-- ═══ Providers ═══
CREATE TABLE IF NOT EXISTS providers (
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

-- ═══ Models ═══
-- A model belongs to one provider: removing a provider that still has models
-- is refused (delete or move its models first).
CREATE TABLE IF NOT EXISTS models (
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

-- ═══ Agent routing order ═══
-- The whole routing policy: an agent's candidates in pos order, nothing
-- appended after the list. Health reorders it (degraded rows sink), never
-- empties it. Rows reference the models catalog; deleting a referenced model
-- is refused (fix lists first; 'disabled' is the soft option).
-- purpose: one ordered list per job. 'chat' drives the agent's turns;
-- 'image'/'audio' pick the model that turns inbound media into text for this
-- agent's sessions. An agent with no rows for a media purpose uses the global
-- capability pick (llm_proc.c model_pick_for_media). A media row is refused
-- unless the model's capabilities include that purpose (trigger below).
-- 'utility' is the list a script's LLM(prompt, {purpose:"utility"}) walks —
-- cheap side work such as a Stop hook's review. An agent with no
-- utility rows is served from its chat list. Any model may be on it.
CREATE TABLE IF NOT EXISTS agent_models (
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
-- Every write path goes through these, so the rule cannot be skipped by a
-- caller that forgets to check (dashboard, config doc, clone, sqlite3 --write).
CREATE TRIGGER IF NOT EXISTS agent_models_capability_bi
BEFORE INSERT ON agent_models
WHEN NEW.purpose IN ('image','audio') AND NOT EXISTS (
  SELECT 1 FROM models m, json_each(CASE WHEN json_valid(m.capabilities)
                                         THEN m.capabilities ELSE '[]' END) c
   WHERE m.id = NEW.model_id AND c.value = NEW.purpose)
BEGIN
  SELECT RAISE(ABORT, 'agent_models: the model''s capabilities do not include this purpose');
END;
CREATE TRIGGER IF NOT EXISTS agent_models_capability_bu
BEFORE UPDATE OF purpose, model_id ON agent_models
WHEN NEW.purpose IN ('image','audio') AND NOT EXISTS (
  SELECT 1 FROM models m, json_each(CASE WHEN json_valid(m.capabilities)
                                         THEN m.capabilities ELSE '[]' END) c
   WHERE m.id = NEW.model_id AND c.value = NEW.purpose)
BEGIN
  SELECT RAISE(ABORT, 'agent_models: the model''s capabilities do not include this purpose');
END;
CREATE TRIGGER IF NOT EXISTS agent_models_window_bi
BEFORE INSERT ON agent_models
WHEN NEW.trim_threshold > (SELECT m.context_window FROM models m WHERE m.id = NEW.model_id)
BEGIN
  SELECT RAISE(ABORT, 'agent_models: trim_threshold is larger than the model''s context window');
END;
CREATE TRIGGER IF NOT EXISTS agent_models_window_bu
BEFORE UPDATE OF trim_threshold, model_id ON agent_models
WHEN NEW.trim_threshold > (SELECT m.context_window FROM models m WHERE m.id = NEW.model_id)
BEGIN
  SELECT RAISE(ABORT, 'agent_models: trim_threshold is larger than the model''s context window');
END;
-- Anthropic's cache lives 5 minutes or an hour; any other lifetime would
-- arm CacheExpiring for a cache that is not there. A new Anthropic chat entry
-- that says nothing gets the hour: the marker that matches the cadence of an
-- agent that waits on people (the write costs 2x base input, not 1.25x).
CREATE TRIGGER IF NOT EXISTS agent_models_lifetime_bi
BEFORE INSERT ON agent_models
WHEN NEW.cache_lifetime NOT IN (300, 3600) AND EXISTS (
  SELECT 1 FROM models m JOIN providers p ON p.name = m.provider_name
   WHERE m.id = NEW.model_id AND p.endpoint_type = 'anthropic')
BEGIN
  SELECT RAISE(ABORT, 'agent_models: cache_lifetime on an Anthropic model is 300 or 3600');
END;
CREATE TRIGGER IF NOT EXISTS agent_models_lifetime_bu
BEFORE UPDATE OF cache_lifetime, model_id ON agent_models
WHEN NEW.cache_lifetime NOT IN (300, 3600) AND EXISTS (
  SELECT 1 FROM models m JOIN providers p ON p.name = m.provider_name
   WHERE m.id = NEW.model_id AND p.endpoint_type = 'anthropic')
BEGIN
  SELECT RAISE(ABORT, 'agent_models: cache_lifetime on an Anthropic model is 300 or 3600');
END;
CREATE TRIGGER IF NOT EXISTS agent_models_lifetime_ai
AFTER INSERT ON agent_models
WHEN NEW.cache_lifetime IS NULL AND NEW.purpose = 'chat' AND EXISTS (
  SELECT 1 FROM models m JOIN providers p ON p.name = m.provider_name
   WHERE m.id = NEW.model_id AND p.endpoint_type = 'anthropic')
BEGIN
  UPDATE agent_models SET cache_lifetime = 3600 WHERE rowid = NEW.rowid;
END;

-- Availability probe cache: what a provider says it can serve, as opposed to
-- what `models` registers for routing. Slim on purpose — an aggregator's
-- catalog is hundreds of rows, and the description/provider blobs it ships
-- must never reach a context window. synced_at drives the 12h TTL.
CREATE TABLE IF NOT EXISTS models_cache (
  provider_name TEXT NOT NULL,
  id TEXT NOT NULL,
  context_length INTEGER,
  prompt_price TEXT,
  completion_price TEXT,
  modality TEXT,
  synced_at INTEGER NOT NULL,
  PRIMARY KEY (provider_name, id)
);

-- ═══ LLM Jobs (transient queue) ═══
CREATE TABLE IF NOT EXISTS llm_jobs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL,
  agent_name TEXT NOT NULL REFERENCES agents(name) ON UPDATE CASCADE,
  recall INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE INDEX IF NOT EXISTS idx_llm_jobs_session ON llm_jobs(session_id);

-- ═══ Extensions ═══
CREATE TABLE IF NOT EXISTS extensions (
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

-- ═══ Agents ═══
-- name is the key everywhere (schema-string-keys decision, 2026-07-18): every
-- agent_name column below is a real FK with ON UPDATE CASCADE, so agent_rename
-- is one UPDATE here and SQLite cascades the rest. foreign_keys=ON (db_open).
-- No ON DELETE clause anywhere: there is no agent-delete verb, so the default
-- NO ACTION refuses deleting an in-use agent — fail-closed until one exists.
CREATE TABLE IF NOT EXISTS agents (
  name TEXT PRIMARY KEY,                      -- the key every agent_name column refers to
  system_prompt TEXT,                         -- prompt template; NULL = the default prompt (tools, memory,
                                              -- skills are added after it)
  description TEXT,                           -- one line saying what the agent is for
  max_iterations INTEGER DEFAULT 25,          -- LLM requests allowed per turn
  max_output_tokens INTEGER,                  -- cap on tokens per reply; NULL = the model's
  shell_timeout INTEGER DEFAULT 30,           -- seconds a shell_exec call may run
  shell_path TEXT,               -- interpreter for shell_exec's -c; NULL = /bin/sh
  containment TEXT DEFAULT 'standard',        -- how tool calls are contained: none | standard |
                                              -- offline (specs/containment.md)
  created_by TEXT REFERENCES agents(name) ON UPDATE CASCADE,
                                 -- creating agent (update_agent authorization); NULL = operator
  hold_until INTEGER,            -- quiesce lease (cclaw rename-agent): while in the
                                 -- future no NEW turn opens for this agent. NULL = no
                                 -- hold. Expiry self-heals — a crashed holder unblocks.
  hold_holder TEXT,              -- who holds the lease ("cli:<pid>"), for the error text
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);

-- ═══ Grants (normalized capabilities) ═══
-- kind: tool | host | read_path | write_path | secret. A 'secret' grant
-- (value = secret name) is what lets the agent have that secret interpolated
-- into, or delivered to, its tool calls (specs/security.md); host bindings
-- (secret_hosts) still limit where it may be sent.
-- approval_mode applies only to kind='tool' rows (Axis B of §4 authority):
--   'silent'       — granted tool runs freely (default)
--   'always'       — every call parks for approval
--   'tool_decides' — predicate decides; absent a predicate, fail-closed to 'always'
-- source: how the row came to exist (later edits are in change_log). No
-- default on purpose — a writer that does not say is refused.
--   approval     — an approved request was applied (request_config, update_agent,
--                  "always allow" on a parked call, a grant from history)
--   operator     — the operator granted it directly (CLI, dashboard)
--   agent_create — given to a new agent when it was defined (baseline, declared,
--                  or copied by clone_from)
--   extension    — a promoted extension's tools, granted to its owner
--   seed         — startup bootstrap (Assistant, agent definitions on disk or in
--                  built-in manifests)
--   capture      — the agent captured or generated this secret itself
--   migration    — written by a schema patch (v60: existing secret use)
--   legacy       — existed before v60; origin unknown
-- approval_id: the approval being applied when the row was written; NULL when
-- none was (a re-grant that revives an expired row takes the new one).
CREATE TABLE IF NOT EXISTS grants (
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

CREATE TABLE IF NOT EXISTS agent_extensions (
  agent_name TEXT REFERENCES agents(name) ON UPDATE CASCADE,  -- the agent the extension is attached to
  extension_name TEXT NOT NULL REFERENCES extensions(name) ON UPDATE CASCADE,  -- the attached extension
  config TEXT,                                -- per-agent extension settings (JSON); NULL = none
  enabled INTEGER NOT NULL DEFAULT 1,         -- 0 = attached but switched off for this agent
  PRIMARY KEY (agent_name, extension_name)
);

-- ═══ Channels ═══
-- status lifecycle: draft → validated → active. extension_install()
-- inserts 'draft'; only `cclaw --channel <name> --check` (→validated) then
-- `--activate` (→active) can reach 'active' — channel_launch_all() only execs
-- that state. A crash-looping channel stays 'active' and is retried on a
-- backoff; 'broken' exists only in rows an older build left (--check lifts it).
CREATE TABLE IF NOT EXISTS channels (
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

CREATE TABLE IF NOT EXISTS channel_state (
  channel_name TEXT NOT NULL REFERENCES channels(name) ON UPDATE CASCADE,
  key TEXT NOT NULL,
  value TEXT,
  PRIMARY KEY (channel_name, key)
);

-- One row per chat (platform conversation id: Telegram chat id, Discord
-- text-channel/thread/DM id, ...) a channel has seen or an operator named.
-- session_id is the pin: the session this chat's messages go to; the session
-- names its agent (sessions.agent_name, immutable). NULL = not routed — the
-- gate drops the chat's messages unless channels.default_agent opens the door
-- (specs/channels.md). A session is pinned by at most one chat.
-- delivery_mode, tool_filter and guidance are the chat's settings, read live
-- every turn by the session this chat pins (tool_filter) or by any session
-- that belongs to the chat (delivery_mode, guidance) — nothing is copied onto
-- a session. A new row starts from defaults, copied once (see each column).
-- (FK to sessions is declared here before the sessions CREATE below —
-- SQLite resolves FK targets at DML time, not CREATE time.)
CREATE TABLE IF NOT EXISTS channel_chats (
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
CREATE UNIQUE INDEX IF NOT EXISTS idx_channel_chats_session
  ON channel_chats(session_id) WHERE session_id IS NOT NULL;

-- ═══ Sessions ═══
CREATE TABLE IF NOT EXISTS sessions (
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
CREATE INDEX IF NOT EXISTS idx_sessions_owner ON sessions(owner_instance) WHERE owner_instance IS NOT NULL;
CREATE INDEX IF NOT EXISTS idx_sessions_cache_armed ON sessions(cache_armed_at)
  WHERE cache_armed_at IS NOT NULL;
-- "children of this session" is asked on every launch, request build and
-- quiescence check; without this SQLite builds a throwaway index per query.
CREATE INDEX IF NOT EXISTS idx_sessions_parent ON sessions(parent_session_id)
  WHERE parent_session_id IS NOT NULL;

-- ═══ Delivery edges (specs/delivery.md) ═══
-- Every outbound edge of a session is a row: the standing parent edge every
-- child gets at creation, the channel edge a session gets when it is bound to
-- a chat (cursor at its leaf), and the one-shot tool_call edge a blocking
-- launch adds on top. A channel edge has no policy of its own (NULL): its
-- policy is the chat's channel_chats.delivery_mode, read live at every
-- boundary ('auto' = turn) — the row keeps only the cursor. policy is
-- one shared vocabulary on every edge:
--   'iteration' — every content-bearing assistant message, as it lands
--   'digest'    — assistant prose since cursor, concatenated, at turn end
--   'turn'      — final assistant message at turn end (the old standard)
--   'quiescent' — final message at turn end iff the whole subtree is settled
--                 (idle, no unconsumed inbox, no llm_jobs, no live tool_calls)
--                 — the default; errors deliver regardless
--   'explicit'  — nothing auto-ships
-- cursor is the last entry id this edge delivered (0 = nothing yet), stamped
-- inside the delivery transaction. Cursor behind an idle session's assistant
-- leaf = delivery owed — the convergence sweep's re-derivation predicate,
-- which the parent_notified_at stamp it replaced could not express for a
-- multi-turn child's lost non-first push. A fired one-shot row is deleted.
-- target_kind 'session' + one_shot reply edges are reserved for milestone 2's
-- session_send/ping (expires_at arrives with it as a forward patch).
CREATE TABLE IF NOT EXISTS delivery_edges (
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

-- ═══ Entries ═══
CREATE TABLE IF NOT EXISTS entries (
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
-- Three indexes only, one per access shape: per-session scans, the payload
-- builder's covering sibling lookup, and time-window budget sums. Branch
-- walks (leaf→root CTEs) join on e.id = rowid — no parent_id index helps
-- them; narrower filters (role, stop_reason) seek the session then read.
CREATE INDEX IF NOT EXISTS idx_entries_session ON entries(session_id, id);
CREATE INDEX IF NOT EXISTS idx_entries_iteration_type ON entries(session_id, iteration_id, type, part_index);
CREATE INDEX IF NOT EXISTS idx_entries_created ON entries(created_at);

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

-- Leaf advance is atomic with insert; a summary (compaction) is excluded: a turn edit
-- inserts it mid-branch (turn_edit.c) and puts the leaf back itself.
CREATE TRIGGER IF NOT EXISTS entries_leaf_ai AFTER INSERT ON entries
WHEN NEW.type != 'compaction'
BEGIN
  UPDATE sessions SET leaf_id = NEW.id, updated_at = unixepoch() WHERE id = NEW.session_id;
END;

-- turn_id carry, atomic with the insert (like leaf advance above). A turn opens
-- when a user entry follows a non-user one — the mid-turn invariant guarantees
-- that only happens at a turn boundary, so the inbox drain's whole batch of user
-- entries lands in one turn and every assistant/tool entry after it inherits the
-- open turn from its parent. Deriving it from the branch (not a session column or
-- a C variable) is what makes it restart-safe: a daemon that dies mid-turn resumes
-- from the same leaf and keeps the same turn_id, with no state that can drift.
-- An explicit turn_id in the INSERT wins (nothing sets one today).
CREATE TRIGGER IF NOT EXISTS entries_turn_ai AFTER INSERT ON entries
WHEN NEW.turn_id IS NULL
BEGIN
  UPDATE entries SET turn_id = COALESCE(
      (SELECT p.turn_id FROM entries p
        WHERE p.id = NEW.parent_id AND NOT (NEW.role = 1 AND p.role <> 1)),
      (SELECT COALESCE(MAX(turn_id), 0) + 1 FROM entries WHERE session_id = NEW.session_id))
    WHERE id = NEW.id;
END;

-- ═══ Tool calls (workflow state — arguments live in entries.content) ═══
CREATE TABLE IF NOT EXISTS tool_calls (
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
CREATE INDEX IF NOT EXISTS idx_tool_calls_entry ON tool_calls(entry_id);
CREATE INDEX IF NOT EXISTS idx_tool_calls_session ON tool_calls(session_id, status);

-- ═══ Raw LLM responses (forensics / shape archive) ═══
-- One row per LLM response (any HTTP outcome). Retention is set by config key
-- 'llm_response_archive_max': >0 keeps the most recent N 'ok' rows plus the most
-- recent N failures (cited "[resp #N]" rows outlive routine traffic), 0 disables
-- archiving, <0 keeps all. body holds the parsed JSONB blob when the response is
-- valid JSON, or the raw text when it isn't; iteration_id joins back to entries.
-- Read bodies with `cclaw resp <id>` — JSONB needs SQLite >= 3.45 and target
-- boxes ship older system CLIs, so the binary that wrote it is the reader.
CREATE TABLE IF NOT EXISTS llm_responses (
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
CREATE INDEX IF NOT EXISTS idx_llm_responses_iteration ON llm_responses(session_id, iteration_id);

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
CREATE INDEX IF NOT EXISTS idx_channel_outbox_pending ON channel_outbox(channel_name, status) WHERE status='pending';

-- Inbound media awaiting capability-routed preprocessing (e.g. voice →
-- transcript). payload is the full channel-emitted message JSON including
-- media.data_b64 — transient by design: a worker turns it into a text-only
-- inbox row and deletes it. Rows only survive a crash (resubmitted at daemon
-- start); attempts caps crash-loop retries.
CREATE TABLE IF NOT EXISTS media_jobs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL,
  source TEXT NOT NULL DEFAULT '',
  payload TEXT NOT NULL,
  attempts INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);

CREATE TABLE IF NOT EXISTS inbox (
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
CREATE INDEX IF NOT EXISTS idx_inbox_pending ON inbox(session_id, consumed) WHERE consumed = 0;

-- ═══ Tools ═══
CREATE TABLE IF NOT EXISTS tools (
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

-- ═══ Hooks ═══
-- Same provenance model as tools. A hook is a script the runtime forks, in
-- the agent's sandbox, when its event happens (specs/hooks.md). Events:
-- Stop, SubagentStop, ContextThreshold, CacheExpiring.
CREATE TABLE IF NOT EXISTS hooks (
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

-- ═══ Memory ═══
-- Two words, used everywhere: a BLOCK is a named container (description,
-- char_limit, placement); a NOTE is one item of text inside a block,
-- addressed by its row id — ids are never reused and never renumbered.
--
-- placement: 'system' blocks render into the system prompt once per session
-- (agent_config.c); 'context' blocks render into the user context block
-- (<RELEVANT_CONTEXT>), materialized at each turn start (llm_payload.c
-- user_context_text).
-- New blocks default to 'context' — the API default in memory_block_create,
-- which every insert path goes through; the column DEFAULT below is inert.
--
-- created_by is history: the agent that made the block (NULL for a block the
-- operator made). It grants nothing. Who SEES a block, and who may write it,
-- is agent_memory_blocks alone: a block attached to several agents is a
-- shared block. There is no global block — "every agent" is a list of
-- attachments, visible and editable.
CREATE TABLE IF NOT EXISTS memory_blocks (
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

-- Which agents have which blocks. read_only here is per attachment: the
-- creator is attached read-write, anyone else read-only unless the operator
-- says otherwise (a writable shared block lets one agent write into
-- another's prompt). An agent addresses a block by label, so no agent may
-- hold two attachments with the same label — memory_block_attach enforces it.
CREATE TABLE IF NOT EXISTS agent_memory_blocks (
  agent_name TEXT NOT NULL REFERENCES agents(name)
             ON UPDATE CASCADE ON DELETE CASCADE,
  block_id   INTEGER NOT NULL REFERENCES memory_blocks(id) ON DELETE CASCADE,
  read_only  INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (agent_name, block_id)
);

-- Notes inside a block, shown oldest first (ORDER BY id). updated_at is shown
-- to the agent as a date beside the id: without it every note looks equally
-- current, and a stale one is indistinguishable from one written yesterday.
CREATE TABLE IF NOT EXISTS memory_notes (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  block_id INTEGER NOT NULL REFERENCES memory_blocks(id) ON DELETE CASCADE,  -- the block the note is in
  text TEXT NOT NULL,                         -- the note
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  updated_at INTEGER NOT NULL DEFAULT (unixepoch())  -- last time the text changed
);
CREATE INDEX IF NOT EXISTS idx_memory_notes_block
  ON memory_notes(block_id, id);

-- ═══ Cron ═══
-- A job's schedule is exactly one of: cron_expr (recurring 5-field),
-- run_at (one-shot unix ts), or interval_s (fixed period seconds). Non-cron
-- jobs carry cron_expr='' (empty sentinel, never NULL — SQLite can't relax
-- the NOT NULL via ALTER). The payload is task and/or script; both empty is a
-- bare wake (the agent pulse).
CREATE TABLE IF NOT EXISTS cron_jobs (
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
-- Job name is the logical identity (cron_set is an upsert by name; ids die
-- with one-shot auto-removal and delete-recreate cycles).
CREATE UNIQUE INDEX IF NOT EXISTS idx_cron_jobs_name ON cron_jobs(agent_name, name);


-- ═══ Approvals ═══
-- approvals is history/audit; expires_at is the park deadline (fail-closed deny).
-- grants is live truth; grants.expires_at is the time-bounded-grant feature.
-- They do not overlap now that once-scope is gone.
CREATE TABLE IF NOT EXISTS approvals (
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
CREATE INDEX IF NOT EXISTS approvals_pending ON approvals(session_id, state) WHERE state='pending';

-- A decision made off the event loop (the web dashboard), waiting for the
-- loop to carry it out. Deciding an approval re-runs tools and advances a
-- session, which only the loop may do — so another thread or process records
-- its decision here and wakes the loop. The first decision for an approval
-- wins; a row lives for milliseconds.
CREATE TABLE IF NOT EXISTS approval_decisions (
  approval_id INTEGER PRIMARY KEY REFERENCES approvals(id) ON UPDATE CASCADE,
  decision    TEXT NOT NULL CHECK (decision IN ('deny','once','always')),
  decided_via TEXT NOT NULL,
  created_at  INTEGER NOT NULL DEFAULT (unixepoch())
);

-- Files an agent has put behind a link (share_file). The token in the URL is
-- the whole capability: whoever has it can read that one file until the row
-- expires. `path` is the file as resolved when it was shared; the web server
-- serves it only while the file opened at that path still resolves to it.
-- Nothing is copied — the row is the share, and expiry is just the clock.
CREATE TABLE IF NOT EXISTS shares (
  token      TEXT PRIMARY KEY,
  agent_name TEXT NOT NULL REFERENCES agents(name) ON UPDATE CASCADE ON DELETE CASCADE,
  path       TEXT NOT NULL,
  name       TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  expires_at INTEGER NOT NULL
);

-- ═══ Sensitivity axis (specs/trust.md) ═══
-- Operator-owned labels on TARGETS, global (not per-agent). The one place a
-- label subtracts from authority: a sensitive host always escalates, and no
-- standing grant satisfies it — the proxy deny-checks these before any allow
-- rule, and dispatch parks every matching call (ALWAYS coerced to ONCE).
CREATE TABLE IF NOT EXISTS sensitive_targets (
  kind  TEXT NOT NULL DEFAULT 'host',        -- only 'host' in v1
  value TEXT NOT NULL,                       -- exact host or '.suffix' rule
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  PRIMARY KEY (kind, value)
);

-- Fail-closed credential rule (specs/trust.md): a secret may only be
-- submitted to hosts it is bound to. A call using a secret with zero
-- bindings is denied inline with the missing pair named; bindings mint from
-- an approved request_config secret_bindings document or `cclaw secret-bind`
-- (D17). For shell/js calls carrying a secret, the call's egress allow-list
-- IS the union of the used secrets' bound hosts, unconditionally.
CREATE TABLE IF NOT EXISTS secret_hosts (
  secret_name TEXT NOT NULL,
  host        TEXT NOT NULL,                 -- exact host or '.suffix' rule
  created_at  INTEGER NOT NULL DEFAULT (unixepoch()),
  PRIMARY KEY (secret_name, host)
);

-- Secret store (specs/security.md): DB-backed secrets, born via the
-- `cclaw secret set` operator verb, the secret_create tool, or a save_secret
-- capture. value is always "enc:<hex(...)>" (never plaintext). Enforcement
-- comes free from secret_hosts having zero rows for a new secret (first use
-- is denied until a requested binding is approved — D17).
-- scope: 'agent' secrets feed {{SECRET:name}} interpolation + child injection;
-- 'system' secrets (provider API keys) are daemon-consumed only and are never
-- loaded into the agent-facing snapshot — an agent cannot interpolate them.
CREATE TABLE IF NOT EXISTS secrets (
  name       TEXT PRIMARY KEY,                    -- ^[A-Z][A-Z0-9_]*$
  value      TEXT NOT NULL,                   -- encrypted; never shown, logged or sent to a model
  source     TEXT NOT NULL DEFAULT 'operator',      -- 'operator'|'generated'|'captured'
  scope      TEXT NOT NULL DEFAULT 'agent'    -- 'agent' (usable in tool calls when granted) or 'system'
                                              -- (provider keys, daemon only)
             CHECK (scope IN ('agent','system')),
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);

-- ═══ Process registry (liveness) ═══
-- One row per live cclaw process (daemon or cli) sharing this DB. Sessions stamp
-- their owner_instance here; owner-scoped recovery reclaims only sessions whose
-- owner is absent (crashed) or stale (no heartbeat within PROCESS_TTL_SEC).
CREATE TABLE IF NOT EXISTS processes (
  instance_id  TEXT PRIMARY KEY,
  pid          INTEGER NOT NULL,
  mode         TEXT NOT NULL,                 -- 'daemon' | 'cli'
  started_at   INTEGER NOT NULL DEFAULT (unixepoch()),
  heartbeat_at INTEGER NOT NULL DEFAULT (unixepoch())
);

-- ═══ Change log (specs/operations.md "Change log") ═══
-- One row per changed row of a configuration table, written by triggers —
-- never by C code — so no write path can skip it. The triggers are generated
-- from the setting/measurement list in src/change_log.c (CHANGE_LOG_COLUMNS):
-- they fire only when a *setting* changes, and old/new_values hold settings
-- only. A secret's value never appears; its row records that it changed.
-- actor: the writing handle (cclaw_actor(): daemon | cli | dashboard |
-- sqlite3 | llm_worker | ...). context: what that handle was doing
-- (cclaw_context(), e.g. 'approval:41'), NULL when nothing was labelled.
-- row_key: JSON of the row's primary key (after the change; before it for a
-- delete). Append-only: cclaw never updates or deletes a row, and the two
-- triggers below refuse it. A write to a logged table from a SQLite that does
-- not define cclaw_actor() (a plain system sqlite3) fails — use
-- `cclaw sqlite3 --write`.
CREATE TABLE IF NOT EXISTS change_log (
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
CREATE TRIGGER IF NOT EXISTS change_log_append_only_upd
BEFORE UPDATE ON change_log
BEGIN
  SELECT RAISE(ABORT, 'change_log is append-only');
END;
CREATE TRIGGER IF NOT EXISTS change_log_append_only_del
BEFORE DELETE ON change_log
BEGIN
  SELECT RAISE(ABORT, 'change_log is append-only');
END;
