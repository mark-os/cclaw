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
  name TEXT PRIMARY KEY,
  base_url TEXT NOT NULL,
  endpoint_type TEXT NOT NULL DEFAULT 'openai',
  api_key_env TEXT NOT NULL DEFAULT '',
  default_model TEXT,
  priority INTEGER NOT NULL DEFAULT 0,
  status TEXT NOT NULL DEFAULT 'healthy',
  -- JSON merged (json_patch, RFC 7386) into every outgoing request body for
  -- this provider as the LAST step of payload assembly, so it can override
  -- anything cclaw built. The escape hatch for provider-specific request
  -- options (e.g. OpenRouter's upstream routing params) — config, never C.
  request_extra TEXT
);

-- ═══ Models ═══
CREATE TABLE IF NOT EXISTS models (
  id TEXT PRIMARY KEY,
  provider_name TEXT NOT NULL,
  model TEXT NOT NULL,
  context_window INTEGER,
  max_output_tokens INTEGER,
  capabilities TEXT DEFAULT '[]',
  -- How this model spells reasoning effort on the wire, as JSON:
  --   {"format": "openrouter|openai|deepseek|gemini-level|gemini-budget",
  --    "levels": {"off":..,"minimal":..,"low":..,"medium":..,"high":..}}
  -- Level values are wire values (strings for effort enums, integers for
  -- budgets); a missing or null level means this model does not support it,
  -- and a request for it clamps to the nearest level that does. NULL column =
  -- use the endpoint's default map (see specs/providers.md).
  effort_map TEXT,
  status TEXT NOT NULL DEFAULT 'healthy',
  degraded_until INTEGER,
  total_requests INTEGER DEFAULT 0,
  total_tokens_in INTEGER DEFAULT 0,
  total_tokens_out INTEGER DEFAULT 0,
  total_cost_nano INTEGER DEFAULT 0,
  consec_failures INTEGER DEFAULT 0,
  last_success_at INTEGER,
  last_error_at INTEGER,
  synced_at INTEGER,
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);

-- ═══ Agent routing order ═══
-- The whole routing policy: an agent's candidates in pos order, nothing
-- appended after the list. Health reorders it (degraded rows sink), never
-- empties it. Rows reference the models catalog; deleting a referenced model
-- is refused (fix lists first; 'disabled' is the soft option).
CREATE TABLE IF NOT EXISTS agent_models (
  agent_name TEXT NOT NULL REFERENCES agents(name) ON UPDATE CASCADE ON DELETE CASCADE,
  model_id TEXT NOT NULL REFERENCES models(id),
  pos INTEGER NOT NULL,
  -- How hard this agent wants this model to think: off|minimal|low|medium|high.
  -- NULL (the default) sends nothing, which is what every model did before the
  -- knob existed. The level is translated to wire form at build time through
  -- models.effort_map, clamping to the nearest supported level.
  reasoning_effort TEXT CHECK (reasoning_effort IS NULL OR reasoning_effort IN
    ('off','minimal','low','medium','high')),
  PRIMARY KEY (agent_name, pos)
);

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
  job_type INTEGER NOT NULL DEFAULT 0,
  status TEXT NOT NULL DEFAULT 'pending',
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE INDEX IF NOT EXISTS idx_llm_jobs_pending ON llm_jobs(status) WHERE status='pending';

-- ═══ Extensions ═══
CREATE TABLE IF NOT EXISTS extensions (
  name TEXT PRIMARY KEY,
  path TEXT NOT NULL,                       -- shared store dir: ~/.cclaw/extensions/<name>
  version TEXT DEFAULT '0.0.0',
  owner_agent TEXT,                         -- who promoted it ('system' for extensions shipped in the binary)
  published INTEGER NOT NULL DEFAULT 0,     -- the single publish flag
  enabled INTEGER NOT NULL DEFAULT 1,
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);

-- ═══ Agents ═══
-- name is the key everywhere (schema-string-keys decision, 2026-07-18): every
-- agent_name column below is a real FK with ON UPDATE CASCADE, so agent_rename
-- is one UPDATE here and SQLite cascades the rest. foreign_keys=ON (db_open).
-- No ON DELETE clause anywhere: there is no agent-delete verb, so the default
-- NO ACTION refuses deleting an in-use agent — fail-closed until one exists.
CREATE TABLE IF NOT EXISTS agents (
  name TEXT PRIMARY KEY,
  system_prompt TEXT,
  description TEXT,
  max_iterations INTEGER DEFAULT 25,
  max_output_tokens INTEGER,
  shell_timeout INTEGER DEFAULT 30,
  shell_path TEXT,               -- interpreter for shell_exec's -c; NULL = /bin/sh
  sandbox_profile TEXT DEFAULT 'standard',
  created_by TEXT REFERENCES agents(name) ON UPDATE CASCADE,
                                 -- creating agent (update_agent authorization); NULL = operator
  hold_until INTEGER,            -- quiesce lease (cclaw rename-agent): while in the
                                 -- future no NEW turn opens for this agent. NULL = no
                                 -- hold. Expiry self-heals — a crashed holder unblocks.
  hold_holder TEXT,              -- who holds the lease ("cli:<pid>"), for the error text
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);

-- ═══ Grants (normalized capabilities) ═══
-- approval_mode applies only to kind='tool' rows (Axis B of §4 authority):
--   'silent'       — granted tool runs freely (default)
--   'always'       — every call parks for approval
--   'tool_decides' — predicate decides; absent a predicate, fail-closed to 'always'
CREATE TABLE IF NOT EXISTS grants (
  agent_name TEXT NOT NULL REFERENCES agents(name) ON UPDATE CASCADE,
  kind TEXT NOT NULL,
  value TEXT NOT NULL,
  approval_mode TEXT NOT NULL DEFAULT 'silent',
  expires_at INTEGER,
  created_at INTEGER DEFAULT (unixepoch()),
  PRIMARY KEY (agent_name, kind, value)
);

CREATE TABLE IF NOT EXISTS agent_extensions (
  agent_name TEXT REFERENCES agents(name) ON UPDATE CASCADE,
  extension_name TEXT NOT NULL,
  config TEXT,
  enabled INTEGER NOT NULL DEFAULT 1,
  PRIMARY KEY (agent_name, extension_name)
);

-- ═══ Channels ═══
-- status lifecycle: draft → validated → active → broken. extension_install()
-- inserts 'draft'; only `cclaw --channel <name> --check` (→validated) then
-- `--activate` (→active) can reach 'active' — channel_launch_all() only execs
-- that state. channel_reap()'s flap detection writes 'broken' on crash-loop;
-- another --check retries validated→active.
CREATE TABLE IF NOT EXISTS channels (
  name TEXT PRIMARY KEY,
  extension_name TEXT NOT NULL DEFAULT '',
  prev_extension_name TEXT,                 -- revert target recorded by channel swap
  type TEXT NOT NULL DEFAULT '',
  binary_path TEXT NOT NULL DEFAULT '',
  status TEXT NOT NULL DEFAULT 'draft',
  pid INTEGER,
  default_agent TEXT REFERENCES agents(name) ON UPDATE CASCADE,
                    -- Channel open-door policy: non-NULL = unknown chats are
                    -- accepted and get a new session bound to this agent;
                    -- NULL = fail-closed (unrouted chats drop + admin notify).
                    -- Grants no send authority — channel_send needs a route.
  default_tool_filter TEXT,
                    -- JSON array of tool names; NULL = unrestricted. Frozen onto
                    -- every session the routing gate creates for an unrouted chat
                    -- (open-door and admin-fallback alike — unrouted is unrouted).
                    -- Routed chats use channel_routes.tool_filter instead.
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);

CREATE TABLE IF NOT EXISTS channel_state (
  channel_name TEXT NOT NULL,
  key TEXT NOT NULL,
  value TEXT,
  PRIMARY KEY (channel_name, key)
);

-- A route pins a chat (platform conversation id) to a session; the session
-- names its agent (sessions.agent_name, immutable). Routes never name an
-- agent directly — channel-wide default agent lives on channels.default_agent.
-- chat_id: Telegram chat id, Discord text-channel/thread/DM id, etc.
-- (FK to sessions is declared here before the sessions CREATE below —
-- SQLite resolves FK targets at DML time, not CREATE time.)
CREATE TABLE IF NOT EXISTS channel_routes (
  channel_name TEXT NOT NULL,
  chat_id TEXT NOT NULL,
  session_id INTEGER NOT NULL REFERENCES sessions(id),
  delivery_mode TEXT NOT NULL DEFAULT 'auto',  -- delivery-policy *template* for the pinned
                                               -- session's channel edge (specs/delivery.md):
                                               -- 'auto' (= quiescent) | iteration | digest |
                                               -- turn | quiescent | explicit. Frozen onto the
                                               -- session's delivery_edges row at its first
                                               -- delivery boundary — later route edits don't
                                               -- retro-apply (tool_filter precedent).
  tool_filter TEXT,                            -- JSON array of tool names; NULL = unrestricted.
                                               -- Copied onto sessions.tool_filter at session
                                               -- creation only — frozen like sub-agent filters;
                                               -- later route edits don't retro-apply.
  system_prompt_suffix TEXT,                   -- appended to the pinned session's system
                                               -- prompt every turn; NULL = nothing appended.
                                               -- Read live (unlike tool_filter): editing it
                                               -- changes the next turn.
  PRIMARY KEY (channel_name, chat_id)
);

-- ═══ Sessions ═══
CREATE TABLE IF NOT EXISTS sessions (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT,
  agent_name TEXT REFERENCES agents(name) ON UPDATE CASCADE,
  channel_name TEXT,
  chat_id TEXT,
  chat_title TEXT,                          -- observed human-readable name of the bound chat
                                            -- ("#general @ My Server", "DM with alice"). Display
                                            -- only: sends always address by chat_id.
  parent_session_id INTEGER DEFAULT -1,
  parent_tool_call_id TEXT,
  depth INTEGER NOT NULL DEFAULT 0,
  tool_filter TEXT,                         -- JSON array of tool names; NULL = unrestricted.
                                            -- Positive scope frozen at spawn, intersected with
                                            -- grants per turn — never grants anything itself.
  state TEXT NOT NULL DEFAULT 'idle',
  owner_instance TEXT,                      -- live owner (processes.instance_id); NULL ⟺ state='idle'
  turn_iteration INTEGER NOT NULL DEFAULT 0,
  turn_context TEXT,                        -- <RELEVANT_CONTEXT> block, materialized once at turn
                                            -- start (llm_proc.c) and reused verbatim by every
                                            -- tool-loop iteration so the request prefix stays
                                            -- byte-stable for prompt caching. NULL = no block.
  leaf_id INTEGER DEFAULT -1,
  compaction_fail_count INTEGER NOT NULL DEFAULT 0,  -- consecutive failed compaction attempts;
                                            -- zeroed on success. At 3 the operator channel gets
                                            -- one notice (llm_proc.c) — the agent is never told.
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  updated_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE INDEX IF NOT EXISTS idx_sessions_owner ON sessions(owner_instance) WHERE owner_instance IS NOT NULL;

-- ═══ Delivery edges (specs/delivery.md) ═══
-- Every outbound edge of a session is a row: the standing parent edge every
-- child gets at creation, the channel edge a chat-bound session gets lazily at
-- its first delivery boundary (frozen from the route template then — the
-- tool_filter precedent), and the one-shot tool_call edge a blocking launch
-- adds on top. policy is one shared vocabulary on every edge:
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
  policy TEXT NOT NULL,
  cursor INTEGER NOT NULL DEFAULT 0,
  one_shot INTEGER NOT NULL DEFAULT 0,   -- blocking reply edge: fires once, then deleted
  UNIQUE(session_id, target_kind, target_ref)
);

-- ═══ Entries ═══
CREATE TABLE IF NOT EXISTS entries (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL,
  parent_id INTEGER NOT NULL DEFAULT -1,
  original_parent_id INTEGER,
  turn_id INTEGER,          -- the turn this entry belongs to. Minted when a user entry
                            -- follows a non-user entry (the inbox drain's first message,
                            -- or a directly appended user entry); every entry after it
                            -- inherits it from its parent until the next turn opens.
                            -- Filled by entries_turn_ai — see the trigger.
  iteration_id INTEGER,     -- one LLM request/response *inside* a turn: minted per request
                            -- by db_next_iteration_id, shared by the assistant message,
                            -- its tool_call parts and their tool results.
  type TEXT NOT NULL DEFAULT 'user_message',
  part_index INTEGER NOT NULL DEFAULT 0,
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
  cached_tokens INTEGER,   -- subset of usage_in the provider served from cache
                           -- (NULL when the provider doesn't report it)
  cost_nano INTEGER,
  token_estimate INTEGER,
  content_bytes INTEGER,
  tool_call_count INTEGER NOT NULL DEFAULT 0,
  data TEXT,
  network_hosts TEXT,  -- NULL or JSON array of hosts the tool run contacted
  -- Reasoning replay blob, set on type='reasoning' entries only:
  -- {provider, model, format, blob}. provider/model tag who produced it
  -- (replay strips on model switch); format is the wire shape —
  -- 'reasoning_details' (OpenRouter array, replayed verbatim: this is how
  -- Gemini thoughtSignature round-trips on the OpenAI-compat path),
  -- 'reasoning_content' (DeepSeek-style bare string), or 'gemini_parts'
  -- ([{fn,sig}] — native thoughtSignature, per functionCall). Replay always
  -- reads $.blob, never entries.content: save_reasoning governs the display
  -- text only, while the wire requirement holds either way.
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

-- Leaf advance is atomic with insert; compaction excluded (entry_compact inserts mid-branch)
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
  request_body BLOB,             -- JSONB of the payload we sent (failures only); NULL on success
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
  name TEXT PRIMARY KEY,
  extension_name TEXT,                       -- NULL for built-in C tools
  description TEXT,
  parameters_json TEXT,
  path TEXT,                                 -- handler file (in the shared store)
  agent_name TEXT REFERENCES agents(name) ON UPDATE CASCADE,
                                             -- owner scope, NULL = global
  enabled INTEGER NOT NULL DEFAULT 1,
  policy TEXT,
  egress_hosts TEXT                          -- comma-separated hosts declared by the
                                             -- manifest ($.tools[].hosts). Non-NULL
                                             -- REPLACES the agent's host grants for
                                             -- calls of this tool (Q1); NULL = the tool
                                             -- runs under the agent's grants. Only a
                                             -- promoted extension tool can carry one.
);

-- ═══ Hooks ═══
-- Same provenance model as tools (replaces the __cclaw_hooks scrape).
CREATE TABLE IF NOT EXISTS hooks (
  extension_name TEXT NOT NULL,
  event TEXT NOT NULL,                        -- one of the six hook events
  path TEXT NOT NULL,                         -- handler file (in the shared store)
  enabled INTEGER NOT NULL DEFAULT 1,
  PRIMARY KEY (extension_name, event, path)
);

-- Per-request hook directives: preAdvance commands that must cross the
-- main-thread → worker-thread boundary as DB state. 'inject' (ephemeral) and
-- 'suppress' rows live for exactly one LLM request — written at dispatch on
-- the poll thread, read by the worker's payload build, deleted at llm_req exit.
CREATE TABLE IF NOT EXISTS hook_directives (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL,
  kind TEXT NOT NULL,              -- 'inject' | 'suppress'
  role TEXT,                       -- inject: 'system' | 'user'
  content TEXT,                    -- inject payload
  entry_id INTEGER,                -- suppress target
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE INDEX IF NOT EXISTS idx_hook_directives_session ON hook_directives(session_id);

-- ═══ Memory ═══
-- placement: 'system' blocks render into the system prompt once per session
-- (agent_config.c); 'context' blocks render into the <RELEVANT_CONTEXT>
-- block, materialized at each turn start (llm_payload.c session_context_text).
-- New blocks default to 'context' — the API default in memory_block_create,
-- which every insert path goes through; the column DEFAULT below is inert.
-- Only the Assistant's seeded AGENT/USER identity blocks ask for 'system'.
CREATE TABLE IF NOT EXISTS memory_blocks (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  agent_name TEXT REFERENCES agents(name) ON UPDATE CASCADE,
  label TEXT NOT NULL,
  description TEXT,
  char_limit INTEGER NOT NULL DEFAULT 5000,
  read_only INTEGER NOT NULL DEFAULT 0,
  placement TEXT NOT NULL DEFAULT 'system',
  created_at INTEGER NOT NULL DEFAULT (unixepoch()),
  updated_at INTEGER NOT NULL DEFAULT (unixepoch()),
  UNIQUE(agent_name, label)
);

-- Numbered entries within a memory block (block = container of entries)
CREATE TABLE IF NOT EXISTS memory_entries (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  agent_name TEXT NOT NULL REFERENCES agents(name) ON UPDATE CASCADE,
  block_label TEXT NOT NULL,
  pos INTEGER NOT NULL,
  text TEXT NOT NULL,
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
);
CREATE INDEX IF NOT EXISTS idx_memory_entries_block
  ON memory_entries(agent_name, block_label, pos);

-- ═══ Cron ═══
-- A job's schedule is exactly one of: cron_expr (recurring 5-field),
-- run_at (one-shot unix ts), or interval_s (fixed period seconds). Non-cron
-- jobs carry cron_expr='' (empty sentinel, never NULL — SQLite can't relax
-- the NOT NULL via ALTER). The payload is task and/or script; both empty is a
-- bare wake (the agent pulse).
CREATE TABLE IF NOT EXISTS cron_jobs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  agent_name TEXT REFERENCES agents(name) ON UPDATE CASCADE,
  name TEXT NOT NULL,
  cron_expr TEXT NOT NULL,
  run_at INTEGER,                            -- one-shot: fire once at this time
  interval_s INTEGER,                        -- fixed period: refire every N seconds
  kind TEXT NOT NULL DEFAULT 'task',         -- always 'task'; legacy DBs held
                                             -- 'heartbeat' (folded into a bare
                                             -- wake job by the v41 patch)
  session_id INTEGER NOT NULL,
  task TEXT NOT NULL,
  script TEXT,                               -- workspace-relative QJS path; runs sandboxed at
                                             -- fire time. NULL = no script payload
  channel_name TEXT,                         -- chat stamp: the durable identity a fire routes
  chat_id TEXT,                              -- through (channel_routes), resolved at fire time
  target TEXT,                               -- NULL = follow the conversation (stamped chat,
                                             -- else stamped session_id); 'pin' = explicit
                                             -- session pin; 'new' = fresh session per fire
  target_agent TEXT REFERENCES agents(name) ON UPDATE CASCADE,
                                             -- agent for 'new'-mode fires; NULL = agent_name
  enabled INTEGER NOT NULL DEFAULT 1,
  next_run_at INTEGER NOT NULL DEFAULT 0,
  last_run_at INTEGER,
  created_at INTEGER NOT NULL DEFAULT (unixepoch())
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
  session_id   INTEGER NOT NULL,
  tool_call_id TEXT,
  tool_name    TEXT,                         -- WHAT parked: the tool that asked
  park_reason  TEXT,                         -- WHY it parked: 'approval_required' (the
                                             -- gate/tool asked) or 'sensitive_target'
                                             -- (specs/trust.md rule 1 — per-call, never
                                             -- satisfiable by a standing grant)
  args_json    TEXT,
  resolve      TEXT NOT NULL DEFAULT 'rerun',
  state        TEXT NOT NULL DEFAULT 'pending',
  decided_via  TEXT,
  requested_at INTEGER DEFAULT (unixepoch()),
  expires_at   INTEGER
);
CREATE INDEX IF NOT EXISTS approvals_pending ON approvals(session_id, state) WHERE state='pending';

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
  value      TEXT NOT NULL,
  source     TEXT NOT NULL DEFAULT 'operator',      -- 'operator'|'generated'|'captured'
  scope      TEXT NOT NULL DEFAULT 'agent'
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
