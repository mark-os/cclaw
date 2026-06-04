# SPEC

## §G GOAL
minimal autonomous AI agent in C, inspired by Pi & OpenClaw — multi-channel (CLI, Telegram), 3-DB SQLite backbone (cclaw.db, per-agent agent.db, journal.db), MicroQuickJS runtime tools, sub-agents, exit-code IPC

## §C CONSTRAINTS
- C11, `-Wall -Wextra -Werror`
- blocking I/O, threads over callbacks, no event loops
- 3-DB split (all WAL mode): `cclaw.db` (registry, permissions, cron, spawn_queue, channels, providers), per-agent `agents/<name>/agent.db` (sessions, entries, inbox, js_tools, memory_blocks, kv), `journal.db` (all logs)
- ⊥ JSON config files — all config in SQLite (cclaw.db for policy, agent env vars for runtime)
- config resolution: `CCLAW_*` env vars > cclaw.db kv > provider-native env vars (e.g. `OPENROUTER_API_KEY`)
- agents write only own DB; communicate intent via exit codes (0=done, 1=error, 2=spawn, 3=approval, 4=config)
- daemon reads agent DB after reap to discover requests; writes agent DB only for inbox delivery
- log collector process — receives stdout/stderr via pipes (`SCM_RIGHTS` fd passing), writes journal.db
- config injected to agents via `CCLAW_*` env vars at fork time (⊥ config file reads in agent)
- system libcurl (dynamic link); vendor cJSON, sqlite3, civetweb, mquickjs
- primary target: EC2 t4g.small ARM64 AL2023
- single user — no multi-tenant auth
- OpenAI-compatible chat completions format ∀ providers
- default provider: OpenRouter → DeepSeek V4 Flash (`deepseek/deepseek-v4-flash`)
- no streaming — full response only (simplifies agent loop, tool parsing)
- daemon+fork model: daemon schedules, forks agent processes, reaps; never executes LLM logic
- agent process = one session turn (drain inbox → LLM loop until stop → write response → exit)
- IPC: exit codes + DB reads (daemon reads agent DB post-reap); signal pipe for wakeup only (no message broker)
- CLI standalone: opens agent DB directly, no daemon needed
- CLI startup: agent picker → session picker; first run creates "default" agent automatically; cclaw.db kv `default_agent` configurable; `-p` uses default agent, creates new session (persisted, ⊥ discarded)
- CLI mode: CWD bind-mounted rw in shell namespace (user watching); daemon mode: workspace rw only
- minimum privilege: default agent starts w/ minimal tools; escalates via `request_config` w/ user approval

## §I INTERFACES
- cmd: `./build/cclaw --cli` → agent picker → session picker → stdin/stdout REPL; opens cclaw.db + agent DB (standalone, no daemon)
- cmd: `./build/cclaw -p "prompt"` → uses kv `default_agent`, new session, print response, exit (session persisted)
- cmd: `./build/cclaw -p "prompt" -s 3` → uses kv `default_agent`, existing session 3, print response, exit
- cmd: `./build/cclaw --log-level=trace` → raw LLM req/resp JSON to stderr
- cmd: `./build/cclaw` → daemon mode (epoll loop: reap children, wake on signal pipe, fork agents; Telegram poller thread + civetweb status page run in-process; spawns log collector at startup)
- cmd: `./build/cclaw --sub-agent --session-id=X --task="..."` → sub-agent process
- env: `OPENROUTER_API_KEY` → seed `provider.api_key` in cclaw.db kv on first run (minimum to start)
- env: `CCLAW_*` → all agent config injected by daemon/CLI at fork as env vars; agent reads via `config_load_from_env()`; pattern: agent_config key `foo_bar` → env `CCLAW_FOO_BAR`; see [specs/schema.md](specs/schema.md) for full key list + defaults
- env: provider API keys injected as native env var (e.g. `OPENROUTER_API_KEY`, `GEMINI_API_KEY`) — daemon decrypts from cclaw.db kv at fork
- env: `CCLAW_SECRET_<NAME>` → decrypted agent secrets (injected at fork; cleared from env after read)
- env: `CCLAW_MODE` → `cli|daemon` (set by parent, not agent_config)
- db: `cclaw.db` — agents registry, agent_config, providers (name, base_url, endpoint_type, api_key_env, default_model_id), kv (encrypted secrets), channel_bindings, tg_chat_sessions, spawn_queue, cron_jobs, approvals, channels, channel_events, channel_outbox, channel_state
- db: per-agent `agents/<name>/agent.db` — sessions, entries, inbox, js_tools, memory_blocks, kv (agent-local prefs only)
- db: `journal.db` — all logs (daemon + agents), source-attributed, timestamped
- db: decryption key `.cclaw/.cclaw_key` — daemon-only, 32 bytes, mode 0600
- exit: agent exit codes → daemon dispatch: 0=deliver response, 1=log error, 2=spawn sub-agent, 3=approval requested, 4=config change, 127=exec failed, 128+N=signal kill
- api: Telegram Bot API (long-poll `getUpdates`, `sendMessage`, `sendChatAction`)
- api: OpenAI-compatible `POST /chat/completions` (any provider)
- web: `GET /` → minimal status page (active sessions, state metrics, inbox depths, uptime)
- tool: `shell_exec` — run cmd, return stdout/stderr
- tool: `file_read` — read file (workspace-restricted)
- tool: `file_write` — write file (workspace-restricted)
- tool: `js_eval` — execute JS in sandboxed mquickjs (plugin engine: agents use this to run workspace scripts, test logic, transform data)
- tool: `js_define_tool` — register JS fn as callable tool (session-persistent); foundation of plugin system — agents augment themselves by defining new tools at runtime
- js binding: `http_fetch(url, {method, headers, body, sanitize})` — C-provided, enforces agent `allowed_hosts` + SSRF protection; `sanitize: true` strips HTML + homoglyphs + boundary wraps (same as web_fetch); sole network path from JS runtime
- tool: `spawn_agent` — agent exits w/ code 2; daemon reads tool_call from agent DB, forks sub-agent
- tool: `db_query` — execute read-only SQL against agent's own DB (SELECT only, no mutations); ⊥ return kv rows where value starts with `enc:`
- tool: `web_fetch` — HTTP GET URL, extract text from HTML, external input protection wrapper
- tool: `memory_create` — create a new memory block (label, description, value?); agent-initiated; default char_limit 5000
- tool: `memory_append` — append text to a memory block by label; respects char_limit + read_only
- tool: `memory_replace` — find-and-replace within a memory block by label; respects char_limit + read_only
- tool: `approval_request` — agent exits w/ code 3; daemon reads proposal from agent DB, notifies admin
- tool: `configure_provider` — agent exits w/ code 4; daemon stores encrypted key in cclaw.db
- tool: `configure_channel` — agent exits w/ code 4; daemon stores + starts listener
- tool: `create_agent` — agent exits w/ code 4; daemon creates agent dir + seeds DB; requires admin approval (V54)

## §D DATA
→ [specs/](specs/) — detailed reference docs:
- [schema.md](specs/schema.md) — 3-DB schema (cclaw.db, agent.db, journal.db), wire emission, design notes
- [daemon.md](specs/daemon.md) — fork architecture, exit code protocol, log collector, env-var injection, turn lifecycle
- [memory.md](specs/memory.md) — memory model, tiers, Letta reference
- [providers.md](specs/providers.md) — auth patterns, cclaw.db storage, wire format differences
- [security.md](specs/security.md) — trust model, config injection best practices, defense-in-depth layers, attack scenarios
- [shell-networking.md](specs/shell-networking.md) — namespace sandbox for shell children, credential proxy, DNS interception
- [error-handling.md](specs/error-handling.md) — LLM API failure classification, retry/fallback logic, response resolution, log levels
- [extensions.md](specs/extensions.md) — extension system (agent-side tools+hooks, channel processes, package layout, API reference)

Notable kv keys (cclaw.db):
- `default_agent` → agent name used by `-p` flag + highlighted in agent picker
- agent_config key `tools` → JSON array of enabled tool names (enforced whitelist); absent = default set per V119 (⊥ all tools)
- agent_config key `allowed_hosts` → JSON array of hostnames; absent = empty (⊥ allow-all); shared by web_fetch + shell networking
- agent_config absent-key rule: missing key = system default (conservative); ⊥ missing = unlimited

## §V INVARIANTS
V1: ∀ tool exec (`file_read`, `file_write`) → path ! ∈ workspace dir for that agent
V2: ∀ LLM call returning 429 → retry w/ exponential backoff, respect `Retry-After`
V3: ∀ sub-agent → max depth 2, max 3 concurrent/parent, max 10 system-wide
V4: ∀ DB access → WAL mode, `busy_timeout` ≥ 5000ms
V5: ∀ `js_eval` → 1MB heap cap, 10M instruction limit
V6: ∀ agent turn → per-turn arena (512KB scratch), destroyed after turn
V7: ∀ context build → load ≤ `context_threshold` × context_window tokens (V91) of most recent turns; if compaction disabled (V93) → prepend truncation notice; older messages remain in DB (searchable via FTS5)
V8: ∀ context build → never cut mid-tool-call; cut at valid turn boundary (before user msg | after complete assistant response)
V9: ∀ LLM request → omit `"tools"` field entirely when no tools registered (never send `"tools": []`)
V10: ∀ tool crash/timeout → produce error result, continue agent loop (never crash loop)
V11: ∀ Telegram msg → chunk at 4096 chars, split at paragraph then sentence boundaries; sleep ≥ 3s between chunks to same chat_id (proactive rate limit — Telegram allows 20 msg/min/chat); backoff (T27) is fallback, not primary flow control
V12: workspace ! per-agent (config `workspace` field per agent, fallback to `./workspace/{agent_name}`)
V13: sub-agent spawn → agent executes tool_calls in order; on reaching `spawn_agent`, writes tool_result entry with `content = "PENDING"` for that tool_call_id, sets state→"waiting", exits code 2; daemon finds PENDING entry (by `content = 'PENDING'`), reads tool_call args from assistant entry, handles spawn; on completion daemon UPDATEs PENDING entry content with real result, transitions state "waiting"→"idle", re-forks agent; agent resumes: executes remaining tool_calls in batch (those without results), then continues LLM loop. Background spawn: same PENDING write, but daemon immediately writes "background agent spawned", re-forks without waiting. Exception: CLI mode uses fork+waitpid in-process (V39)
V14: session tree structure → entries w/ `id` + `parent_id` (Pi model); branching structure in DB even if branching UI deferred
V15: ∀ tool result → optionally wrap in `<tool_result name="X">...</tool_result>` tags to sanitize external data (configurable per tool)
V16: ∀ agent_run → session state machine (idle|running|waiting) is the lock; daemon is sole fork-parent → no concurrent agents per session possible → CAS unnecessary; `lock_holder`/`lock_acquired_at` columns removed
V17: ∀ turn → entries share a `turn_id`; incomplete turns intercepted: (1) if tool_result entry has `content = "PENDING"` on agent startup → daemon failed to deliver; replace with "error: daemon did not deliver result"; (2) if assistant entry has tool_calls with no corresponding tool_result entries and session was NOT in "waiting" state → crash recovery, synthesize failure notice via `context_build`; (3) if assistant has tool_calls with some results present and remaining calls have no results → partial turn resume, agent executes remaining calls (V13)
V18: ∀ inbox message → consumed exactly once into session entries via single atomic SQLite transaction (`BEGIN EXCLUSIVE`)
V19: ∀ session state transition → executed via strict atomic UPDATE with WHERE clauses targeting expected states to prevent TOCTOU
V20: ∀ session → `agent_name` identifies agent; config loaded from `agents/<name>/` on disk; fallback to global config when NULL
V21: ∀ agent execution → daemon forks dedicated process; daemon ⊥ executes LLM logic
V22: ∀ agent process → trusted binary; primary isolation: `setrlimit` (V23) + application-level network policy (V46); agent process is NOT namespace-sandboxed (needs libcurl, own DB, cclaw.db read)
V22a: ∀ `shell_exec` child → full namespace sandbox: `unshare(CLONE_NEWUSER|CLONE_NEWNS|CLONE_NEWNET)`; CLI mode: CWD rw + workspace rw + system dirs ro; daemon mode: workspace rw + system dirs ro; network via `LD_PRELOAD=libcclaw_net.so` routing through UDS to agent proxy thread; proxy enforces allowed_hosts + secrets injected via env vars
V23: ∀ agent process → `setrlimit` at fork: RLIMIT_AS (agent_config `memory_limit`, default 256MB, 0=unlimited, skipped on Android), RLIMIT_CPU (agent_config `cpu_limit`, default 300s, 0=unlimited), RLIMIT_NOFILE (64); limits inherited by shell children
V24: ∀ session → at most 1 active agent process; daemon forks only when state == "idle"; "running" and "waiting" block new forks
V25: ∀ channel inserter (Telegram, cron, sub-agent) → inbox_insert + write session_id to signal pipe; ⊥ run agent logic
V26: ∀ agent process exit → daemon reaps via `waitpid`, reads `last_route` from session, delivers response to channel
V27: ∀ inbox consumption → agent updates `last_route` from newest item's `source` field
V28: ∀ context_build → skip assistant entries w/ `stop_reason` ∈ {"error","aborted"} (per V36); their tool_calls excluded from pending tracking (no synthetic tool_results for dead calls)
V29: ∀ LLM response → missing `finish_reason` (truncated stream, socket broken) → treat as error, write entry w/ `stop_reason: "error"` + partial content preserved, retry per V2
V30: ∀ agent process crash (SIGKILL, OOM, RLIMIT_CPU) → no entry written; daemon reaps; next fork for session → V17 detects incomplete turn, synthesizes failure notice
V31: ∀ daemon shutdown (SIGTERM) → forward signal to active children; children write error entry if possible; unfinished turns recovered via V17 on next startup
V32: ∀ LLM error retry → `continue` semantics: re-send last valid context state (after V28 stripping); max 3 retries per turn before writing final error entry + exiting
V33: ∀ blocking sub-agent completion → daemon reaps sub-agent, reads result from sub-agent's DB, UPDATEs parent's PENDING tool_result entry (`WHERE tool_call_id = ? AND content = 'PENDING'`) with real result → daemon transitions parent state "waiting"→"idle" → daemon re-forks parent
V34: ∀ agent process → `prctl(PR_SET_PDEATHSIG, SIGTERM)` after fork; daemon death auto-kills children; daemon startup recovery: reset all "running"→"idle" (children already dead); "waiting" w/ PENDING entry still present → leave as-is (V17 case 1 handles on next agent fork: PENDING replaced with error); "waiting" w/ no PENDING (result already delivered) → "idle"
V35: ∀ LLM response → normalize provider `finish_reason` → `StopReason` enum (`stop|length|tool_use|error|aborted`) before storing entry; `map_stop_reason()` in `llm.c` is sole normalization point
V36: ∀ context_build → skip assistant entries w/ `stop_reason ∈ {error, aborted}`; synthesize error tool_results for their orphaned tool_calls (`"error: process terminated during execution"`); never send errored/aborted turns to LLM
V37: ∀ `shell_exec` child → namespace sandbox (`CLONE_NEWUSER|CLONE_NEWNS|CLONE_NEWNET`); filesystem restricted to workspace rw + `/` ro; network via `LD_PRELOAD=libcclaw_net.so` routing through UDS to agent proxy thread; proxy enforces allowed_hosts allowlist; secrets via env vars; `CLONE_NEWNET` hard backstop for anything bypassing preload; graceful fallback: if namespaces unavailable, run unsandboxed (log warning)
V38: ∀ JS runtime (`js_eval`, `js_define_tool`) → C-provided `http_fetch(url, opts)` binding is sole network path; binding enforces per-agent `allowed_hosts` allowlist + SSRF protection (reject private IPs) before calling libcurl; no allowlist = no network from JS
V39: ∀ `spawn_agent` in CLI mode → fork+exec+waitpid in-process (blocking) or fork+continue (background); CLI has no daemon — ⊥ use "exit into waiting state" pattern; tool_subagent must detect execution mode and branch accordingly
V40: ∀ tool_result content > 50KB or 2000 lines → truncate before storing in DB entry; full output written to `/tmp/cclaw-<session_id>/<tool_call_id>.out`; truncated result gets suffix `[truncated — showing last {N} lines of {M}. Full output: /tmp/cclaw-<session_id>/<tool_call_id>.out]`; agent can `file_read` the full output path if needed; temp dir cleaned on session idle timeout or daemon restart
V41: ∀ LLM request → built via `CURLOPT_READFUNCTION` streaming from SQLite cursor; ⊥ load full session into memory; two-pass: (1) plan entry IDs + cut point (uses integer columns only — ⊥ touch content/tool_calls), (2) stream wire JSON from cursor via `json_escape_into` + snprintf (⊥ cJSON parse on hot path); per-agent memory footprint ≤ arena + curl buffers (~2-5MB), not session-proportional
V42: ∀ heartbeat → daemon triggers agent run w/ heartbeat prompt; agent reads `HEARTBEAT.md` if present, acts on tasks; response `HEARTBEAT_OK` = sentinel (suppressed, ⊥ delivered to channel); any other response → deliver to channel via `last_route`
V44: ∀ Telegram group msg → if agent response contains `[NO_REPLY]` → suppress delivery (⊥ send to chat); agent decides relevance per system prompt guidance
V45: ∀ agent response → if `stop_reason == stop` & no tool_calls & response is plan-only (bullet list + "I'll do X" promise, no tool action taken) → re-prompt once: "Do not restate the plan. Act now: take the first concrete tool action. If blocked, state the blocker in one sentence."
V46: ∀ outbound HTTP (libcurl) → single `http_policy` layer validates before connect; two modes: (1) default-deny: `allowed_hosts[]` non-empty → only listed hosts reachable, (2) default-allow: `allowed_hosts[]` empty → all hosts reachable except `blocked_hosts[]`; `block_private` configurable per-agent (default true — blocks RFC1918/loopback/link-local + cloud metadata 169.254.169.254); `blocked_hosts` loadable from plain text file (one domain/line, compatible w/ abuse.ch/Pi-hole domain-only format); loaded into hash set at startup; all callers share one code path: LLM calls pass trusted policy (provider endpoints only), web_fetch/JS fetch pass agent's policy
V47: ∀ `shell_exec` child → PATH restricted to `/bin:/usr/bin`; unset `OPENROUTER_API_KEY`, `GEMINI_API_KEY`, `HOME`, and all `CCLAW_*` env vars before exec; prevents agent from invoking cclaw binary or leaking credentials via shell
V48: ∀ `shell_exec` child → `mjs` binary available at fixed path (e.g. `/usr/local/lib/cclaw/mjs`); PATH ! include its directory — agent invokes via absolute path; `mjs` = standalone mquickjs evaluator w/ `fetch()` binding; convenience for shell pipelines (not a security boundary — shell children get full networking via namespace+proxy)
V49: ∀ `mjs` process (spawned via shell_exec) → inherits shell child's namespace sandbox (V82); network goes through credential proxy like any other shell process; `fetch()` binding calls libcurl in-process; composable with unix pipes (V51)
V50: [removed — fetch proxy protocol replaced by in-process whitelist]
V51: ∀ `mjs` invocation via shell → composable w/ unix pipes; stdout/stderr flow through shell pipeline normally; no side-channel fd needed
V52: ∀ secret → all secrets stored in cclaw.db `kv` table (encrypted, ChaCha20-Poly1305 AEAD); two tiers: (1) provider/channel secrets (API keys, bot tokens) — daemon-managed; (2) agent-specific secrets (tool credentials, user tokens) — agent requests via exit code 4, daemon encrypts + stores after admin approval; decryption key `.cclaw/.cclaw_key` (32 bytes, mode 0600) held by daemon only; at fork, daemon decrypts needed secrets + injects into child env; agent clears from env immediately after read; shell_exec children ⊥ access (env stripped before exec)
V53: ∀ Telegram admin command (config, key, whitelist) → restricted to `admin_chat_ids` in kv table; unauthorized chat_id → silent ignore (⊥ error msg, ⊥ inbox_insert)
V54: ∀ config/permission mutation → requires admin approval; agent proposes via `approval_request` tool (exit code 3); daemon reads proposal, delivers to admin (inline keyboard); admin approve → daemon writes config to cclaw.db agent_config; admin deny → posts denial to agent inbox; config changes take effect on next fork (daemon re-reads agent_config)
V55: ∀ memory block edit → agent tools modify `value` only; `label`, `description`, `char_limit`, `read_only` immutable from agent; `value` length ! ≤ `char_limit`; blocks persist across sessions (agent-scoped, not session-scoped); default blocks on agent creation: `AGENT` (identity/tone) + `USER` (facts about user); deletable, editable by agent by default
V56: ∀ `context_plan` query → ⊥ read `content` or `tool_calls` columns; use `role`, `stop_reason`, `tool_call_count`, `token_estimate` (all integer/small columns on main B-tree page); avoids overflow page loads during plan pass
V57: ∀ SQLite open (agent process) → `PRAGMA mmap_size` ≥ 64MB; eliminates double-buffering (SQLite userspace cache + kernel page cache); kernel page cache shared across daemon + forked agents; `PRAGMA cache_size = -512` (reduce userspace cache when mmap active)
V58: ∀ session compaction → reparent: append summary entry w/ `parent_id` = last kept entry; `UPDATE entries SET parent_id = <summary_id> WHERE id = <first_compacted_tail_entry>`; old entries remain in DB (searchable via FTS5, reachable via forward walk from branch point); CTE from leaf stops at summary node → ⊥ walk compacted prefix
V59: ∀ reparented entry → store `original_parent_id` (nullable); enables undo of compaction surgery; NULL = never reparented
V60: ∀ entry wire emission → ⊥ cJSON parse; `content` emitted via `json_escape_into()` (linear pass, no alloc); `tool_calls` emitted via per-provider formatter that walks stored JSON array w/ minimal parse (extract `id`, `name`, `args` substring offsets) — ⊥ full DOM build; `args` object emitted verbatim for Anthropic/Google, stringified (escaped) for OpenAI
V61: ∀ agent kv table → agent-local preferences only (display settings, tool state, scratch data); ⊥ policy config (that's cclaw.db agent_config per V80); `kv_get(key)` / `kv_set(key, val)` for agent's own use; no `enc:` values in agent kv (secrets live in cclaw.db only per V67)
V62: ∀ agent → owns dedicated SQLite file at `.cclaw/agents/<name>/agent.db`; contains: sessions, entries, inbox, kv, memory_blocks, js_tools — all scoped to that agent; agent has full RW; daemon has full RW to all agent DBs (V70); child agents ⊥ access parent DB (namespace sandbox blocks shell children; agent binary only opens own `CCLAW_AGENT_DB`); workspace at `.cclaw/agents/<name>/workspace/`
V63: ∀ parent→child agent visibility → parent can read child's DB (daemon facilitates via ATTACH or direct read); child ⊥ read parent DB; enforced by: agent binary only opens `CCLAW_AGENT_DB` (trusted code); shell children namespace-sandboxed to own workspace
V64: ∀ agent config (model, allowed_hosts, tools, max_iterations) → stored in cclaw.db `agent_config` table (V80); daemon reads at fork, injects as env vars; daemon validates config sanity before fork (e.g. allowed_hosts ⊥ include private IPs); config changes require admin approval (V54)
V65: ∀ agent → owns dedicated dir `.cclaw/agents/<name>/` (agent.db + workspace/); ⊥ deleted — audit trail preserved; all agents are siblings (flat hierarchy under `.cclaw/agents/`); agents created explicitly (CLI, `create_agent` tool, templates); `spawn_agent` w/ name launches existing agent (⊥ creates new one)
V66: ∀ agent permissions → agent process opens only own DB + workspace (trusted binary behavior); shell children namespace-sandboxed to workspace only; `read_access` config field: daemon can grant RO access to other agent dirs (passed via env, agent binary honors it); creator→child: daemon grants RW by default; child→creator: nothing; sibling→sibling: nothing
V67: ∀ secrets + provider config → top-level (daemon-owned); stored in cclaw.db `kv` table encrypted (lowercase keys, e.g. `openrouter_api_key`); daemon decrypts + injects as provider-native env var at fork (e.g. `OPENROUTER_API_KEY`); agents read via `getenv(cfg->provider.api_key_env)`; agents ⊥ store provider keys in own DB — only daemon holds them
V68: ∀ bootstrap (first run, no named agents) → daemon spawns ephemeral agent w/ onboarding-focused system prompt; same tools as named agents (`configure_provider`, `configure_channel`, `create_agent`, etc.); ephemeral walks user through: provider setup (API key → daemon stores encrypted), channel setup (Telegram token etc.), agent creation (name, model, persona); agent creation requires admin approval
V69: ∀ channel→agent binding → daemon routes channel messages to bound agent; on agent creation, creator can bind new agent to a channel (replaces previous binding); next message on that channel wakes the new agent; old binding (e.g. ephemeral) becomes inactive (sessions preserved, agent ⊥ deleted)
V70: ∀ daemon → unsandboxed; full RW to all agent DBs; writes: inbox_insert (message delivery), session state transitions, approval resolution + config writes on approve, agent creation seeding; reads: agent config at fork, approval proposals, session state; daemon is sole process w/ cross-agent write access
V71: ∀ LLM request → daemon tracks rolling token usage (input+output) per hour; if hourly total exceeds `token_rate_limit` (default 1000000, configurable via kv + env `CCLAW_TOKEN_RATE_LIMIT`) → reject agent fork w/ error "token rate limit exceeded"; resets on rolling 1h window; tracked in daemon memory (not persisted — resets on daemon restart)
V72: ∀ agent process → exit code is sole IPC channel for intent: 0=turn complete (deliver), 1=error, 2=spawn sub-agent, 3=approval requested, 4=config change requested, 127=exec failed, 128+N=killed by signal N; daemon dispatches on code after `waitpid`; exit codes 2/3/4 imply partial turn — agent executed tool_calls in order up to the daemon-requiring call, wrote PENDING entry for it, remaining calls deferred to next fork
V73: ∀ DB access → 3-file split: `cclaw.db` (registry, permissions, cron, spawn_queue, channels, providers), per-agent `agents/<name>/agent.db` (sessions, entries, inbox, js_tools, memory_blocks, kv), `journal.db` (all logs); agent process opens only own agent.db (+ optional RO cclaw.db if granted)
V74: ∀ agent process → config from `CCLAW_*` env vars injected by daemon at fork; ⊥ read config files; ⊥ open cclaw.db for config (unless `daemon_db_read=1` in agent_config)
V75: ∀ log output (daemon + agents) → piped to log collector process; collector receives fds via `SCM_RIGHTS` over unix socketpair; writes to journal.db (source, pid, session_id, stream, line, timestamp); flush every 100ms or 64 lines
V76: ∀ cclaw.db write → daemon process only; agents ⊥ write cclaw.db; daemon writes agent DB only for inbox delivery
V77: ∀ daemon reap (exit code 2) → open agent DB, find tool_result entry with `content = 'PENDING'`, read matching tool_call from last assistant entry (by tool_call_id), parse spawn_agent args, insert spawn_queue in cclaw.db, fork sub-agent; on sub-agent completion UPDATE the PENDING entry with result
V78: ∀ daemon reap (exit code 3) → open agent DB, find tool_result entry with `content = 'PENDING'`, read matching tool_call (by tool_call_id), parse approval args, insert approvals in cclaw.db, notify admin; on admin resolution UPDATE the PENDING entry with result ("approved"/"denied: reason"), transition state→"idle", re-fork
V79: ∀ daemon reap (exit code 4) → open agent DB, find PENDING tool_result entry, read matching tool_call, validate config change, apply to cclaw.db (provider/channel/agent creation); UPDATE PENDING entry with result ("configured"/"error: ..."), transition state→"idle", re-fork
V80: ∀ agent_config → stored in cclaw.db `agent_config(agent_name, key, value)` table; ⊥ agent.json files; daemon reads at fork, injects as env vars; keys: model, workspace, tools, allowed_hosts, read_access, max_iterations, shell_timeout, daemon_db_read
V81: ∀ CLI mode → mini-daemon: opens cclaw.db, loads config; agent picker (list agents from cclaw.db, create "default" on first run); session picker (first_prompt[:50], last_prompt[:50], datetime); forks `agent_turn_run` per turn; dispatches on exit codes (0=deliver, 3=approval prompt inline, 4=config apply); `CCLAW_MODE=cli`; `CCLAW_PATH`=CWD (file_read ro access); CWD bind-mounted rw in shell namespace
V82: ∀ `shell_exec` child → transparent credential proxy: `LD_PRELOAD=libcclaw_net.so` intercepts `connect()`/`getaddrinfo()`, routes through UDS to agent proxy thread; `CLONE_NEWNET` as hard backstop (static binaries / raw syscalls get zero network); secrets injected via `CCLAW_SECRET_*` env vars (never in command string); output masked before DB write; graceful fallback if namespaces unavailable
V83: ∀ credential proxy → thread in agent process; listens on UDS (`<workspace>/.proxy.sock`); reads preamble (dest host:port) from `libcclaw_net.so` connect; checks `allowed_hosts` allowlist; resolves DNS on behalf of child; opens real TCP connection + relays bidirectionally; denies unlisted hosts; dies with agent process
V84: ∀ credential mapping → stored in cclaw.db `credential_mappings(agent_name, secret_name, host_pattern, path_pattern, location)`; requires admin approval (V54); agent references secrets by name only; proxy resolves + injects at network boundary
V85: ∀ daemon startup → verify user namespaces: `/proc/sys/user/max_user_namespaces` > 0, test `unshare(CLONE_NEWUSER)` succeeds; enforce max concurrent agents ≤ `max_user_namespaces / 6`; warn if kernel constrains below V3 limit
V86: ∀ `shell_exec` child → DNS intercepted via `libcclaw_net.so` (`getaddrinfo()` override); proxy resolves on behalf, caches domain→IP mapping; on TCP connect, proxy uses preamble host for allowlist check; hardcoded IPs (no DNS, no preload intercept) blocked by `CLONE_NEWNET` (no real network path exists)
V87: ∀ partial turn resume → agent on startup checks last assistant entry's tool_calls against existing tool_result entries; if all results present (none PENDING) → normal LLM loop; if PENDING entry found → replace with error (V17 case 1); if results missing (no PENDING, no error) → execute missing tool_calls in order by matching tool_call_id, store results, then continue LLM loop; agent never re-executes a tool_call that already has a result entry
V88: ∀ secret injection to shell → daemon decrypts secrets at fork, injects as `CCLAW_SECRET_<NAME>` env vars into agent process; agent selectively passes needed secrets into shell child env; LLM references as `$CCLAW_SECRET_<NAME>` in commands (value ⊥ in command string, ⊥ in entries, ⊥ in LLM context); shell output scanned for known secret values → replaced w/ `[REDACTED:<name>]` before DB write; LLM told secret names only (never values)
V89: ∀ provider → configured in cclaw.db `providers` table (name, base_url, endpoint_type, api_key_env, default_model_id); multiple providers coexist; agent config specifies `provider` name + optional `model_id`; if model_id omitted → use provider's `default_model_id`; daemon injects ALL configured provider API keys at fork (agent may use multiple providers within a single turn — fallback, sub-calls, etc.); daemon resolves primary provider row at fork, injects base_url/endpoint_type/model as `CCLAW_*` env vars
V90: ∀ config resolution → priority: (1) `CCLAW_*` env vars, (2) cclaw.db kv/agent_config tables, (3) provider-native env vars (e.g. `OPENROUTER_API_KEY`); agent process reads only env vars — resolution happens in parent (CLI/daemon) before fork via `config_load(db)`
V91: ∀ session context management → three configs: `context_threshold` (float 0–1, default 0.6) triggers action when tokens exceed threshold × context_window; `compaction_target` (float 0–1, default 0.3) = how much to keep after compaction (summarize from start until remaining ≤ target × context_window); `compaction` (bool, default true) controls mode
V92: ∀ compaction (enabled) → read entries from session start through compaction_target cut point (entries whose cumulative tokens from tail exceed target × context_window); send to LLM for summarization; replace w/ single ROLE_COMPACTION entry; reparent per V58; fallback to placeholder on LLM failure
V93: ∀ truncation (compaction disabled) → context_build loads most recent entries up to threshold; inserts notice "[conversation history truncated — N earlier entries omitted]" at cut point; no DB mutation — entries remain, just excluded from context sent to LLM
V94: ∀ LLM response w/ HTTP 200 + `usage.total_tokens == 0` + content null/empty + `finish_reason == "stop"` → zero-usage provider glitch; retry 2x primary (same request, no state mutation), then 1x fallback model if configured; on exhaust write error entry w/ `stop_reason=error`; ⊥ store empty response in session
V95: ∀ response delivery → `get_response_text(db, session_id)` is sole resolution fn; walks branch backward from leaf, returns first non-empty assistant content, stops at user boundary; all channels (CLI, Telegram, sub-agent result) use this fn; ⊥ inline branch-walking
V96: ∀ agent child process (CLI & daemon) → stderr piped to journal.db; CLI additionally tees to terminal; daemon pipes via log_collector (existing); unified: both paths persist to journal
V97: ∀ agent process → `CCLAW_LOG_LEVEL` env var (info|debug|trace); info=errors+warnings+turn boundaries; debug=+tool dispatch+timing+retry decisions; trace=+full req/resp JSON; stored in cclaw.db kv `log_level`, injected at fork
V98: ∀ channel → separate process; crash ⊥ affect daemon; daemon launches configured channels on startup, monitors via SIGCHLD, restarts on unexpected exit (max 3 retries w/ backoff)
V99: ∀ channel process → limited cclaw.db access via channel API only: `channel_emit(ctx, payload)`, `channel_get_config(ctx, key)`, `channel_next_outbox(ctx)`, `channel_ack_outbox(ctx, id)`, `channel_fail_outbox(ctx, id, error)`; ⊥ arbitrary SQL
V100: ∀ channel→daemon signaling → `channel_emit` inserts `channel_events` row in cclaw.db + calls `daemon_wake()` (writes to named FIFO at `<db_path>.pipe`); daemon epoll wakes, reads `channel_events`, routes to agent inbox
V101: ∀ daemon→channel delivery → after agent reap, daemon inserts `channel_outbox` row (channel_name, session_id, payload); channel process polls via `channel_next_outbox(ctx)` → delivers → `channel_ack_outbox(ctx, id)` or `channel_fail_outbox(ctx, id, error)`
V102: ∀ channel process → receives config via `channel_get_config(ctx, key)` reading from `channel_state` kv table in cclaw.db (scoped to channel_name); daemon seeds config on channel creation (e.g. bot_token, webhook_url)
V103: ∀ built-in channel (Telegram) → same process model as extension channels; `src/channel_telegram.c` compiled as channel binary; daemon launches same as user-created channels; no special in-process thread
V104: ∀ channel registration → `channels` table in cclaw.db: (name TEXT PK, type TEXT, binary_path TEXT, status TEXT DEFAULT 'active', pid INTEGER); daemon reads on startup, forks each active channel
V105: ∀ `daemon_wake()` → write 1 byte to named FIFO (`<db_path>.pipe`); replaces `daemon_signal_external`; internal signal_pipe remains for in-process use (cron, spawn_queue); FIFO is sole external→daemon wake path
V106: ∀ channel_events → table in cclaw.db: (id INTEGER PK, channel_name TEXT, event_type TEXT, payload TEXT, created_at INTEGER DEFAULT unixepoch()); daemon consumes in order, routes based on event_type + channel_bindings
V107: ∀ channel_outbox → table in cclaw.db: (id INTEGER PK, channel_name TEXT, session_id INTEGER, payload TEXT, status TEXT DEFAULT 'pending', created_at INTEGER, acked_at INTEGER); channel process reads pending rows for own name
V108: ∀ channel_state → kv table in cclaw.db: (channel_name TEXT, key TEXT, value TEXT, PRIMARY KEY(channel_name, key)); channel-private persistent state (offsets, cursors, tokens)
V109: ∀ extension → JS module in `workspace/extensions/` (file `*.js` or subdir w/ `index.js`); loaded fresh each turn; factory fn receives `cclaw` API object; failure (throw) → skip + log warning, continue turn
V110: ∀ extension loading → discovery order: `workspace/extensions/*.js` then `workspace/extensions/*/index.js`; all discovered extensions loaded into shared QuickJS context (same runtime as `js_define_tool`); load order = alphabetical by filename
V111: ∀ extension API → `cclaw.registerTool({name, description, parameters, handler})`, `cclaw.registerHook(event, fn)`, `cclaw.callTool(name, args)` (synchronous, re-entrant); hooks: `beforeRequest`, `afterResponse`, `beforeToolCall`, `afterToolCall`, `turnStart`, `turnEnd`
V112: ∀ `beforeRequest` hook → receives messages array (mutable copy); can modify/add/remove messages before LLM call; return modified array or void (no change); multiple hooks chain in load order
V113: ∀ `beforeToolCall` hook → receives `{name, args}`; can return `{block: true, reason}` to prevent execution, or mutate `args` in place; first block wins
V114: ∀ `afterToolCall` hook → receives `{name, args, result}`; can return replacement `{result}` or void; chains in load order (each sees previous result)
V115: ∀ extension permissions → inherit agent's permissions (allowed_hosts, workspace fs); ⊥ per-extension restrictions; extensions share agent's QuickJS heap (V5 limits apply to total)
V116: ∀ extension `callTool(name, args)` → synchronous dispatch into C tool registry; returns result string; re-entrancy allowed (JS → C tool → JS); depth limit 8 to prevent infinite recursion
V117: ∀ first run (no agents in cclaw.db) → create agent "default" w/ standard config; tools=[file_read, file_write, js_eval, memory_create, memory_append, memory_replace, request_config]; set kv `default_agent`="default"; agent persisted in cclaw.db agents table + `.cclaw/agents/default/agent.db`
V118: ∀ CLI agent picker → list agents from cclaw.db (numbered) + "new agent..." option; on select → session picker for that agent: (id, first_prompt[:50], last_prompt[:50], created_at formatted) + "new session" option
V119: ∀ default agent initial tools → minimum set: `file_read`, `file_write`, `js_eval`, `memory_create`, `memory_append`, `memory_replace`, `request_config`; system prompt enumerates requestable tools (shell_exec, web_fetch, db_query, js_define_tool) so agent knows to escalate
V120: ∀ `request_config` tool (CLI mode) → agent proposes tool grant | host grant; CLI prompts user inline (approve/deny); approved grants persisted in agent_config (cclaw.db); take effect immediately (next iteration, ⊥ re-fork needed — config reloaded in-process)
V121: ∀ host allowlist → shared by web_fetch + shell_exec networking; single config key `allowed_hosts` in agent_config; granting host enables both tools to reach it; tool enablement (web_fetch available) orthogonal to host grants
V122: ∀ new agent creation → inherits default agent config unless cloning existing agent; default config: tools=[file_read, file_write, js_eval, memory_create, memory_append, memory_replace, request_config], allowed_hosts=[], max_iterations=25, shell_timeout=30; clone: copies source agent's full agent_config rows
V123: ∀ sub-agent spawn → two modes: (1) no name: child session in parent's agent.db, same agent identity, own process; inherits parent's full config verbatim; (2) named: launches existing agent by name (error if ! exist); daemon reads that agent's config, enforces parent ceiling (tools ⊆ parent.tools, allowed_hosts ⊆ parent.allowed_hosts, max_iterations ≤ parent.max_iterations); child runs in own agent.db
V124: ∀ agent_config defaults → principle of least surprise: (1) no config key = use system default (not "allow everything"); (2) `tools` absent = default tool set per V119 (⊥ all tools); (3) `allowed_hosts` absent = empty (⊥ allow-all); (4) system defaults defined once in code (`AGENT_DEFAULT_*` constants) + documented in specs/schema.md
V125: ∀ `Authorization` header construction → heap-allocate based on `strlen(api_key)`; ⊥ fixed-size stack buffer; assert header fits before use
V126: ∀ LLM fallback attempt → `http_response_free(resp)` before passing `resp` to next provider; ⊥ memset over live allocation
V127: ∀ outer LLM retry loop (`MAX_LLM_RETRIES`) → only `continue` on parse failure | missing finish_reason; `break` on HTTP errors already exhausted by inner retry+fallback chain; ⊥ re-retry server errors with reset backoff

## §T TASKS
id|status|task|cites
T1-22|x|Core foundation: Makefile, arena, types, config, DB init, sessions, entries, FTS5, HTTP, LLM req/resp, context manager, agent loop, tool registry, max iterations, CLI REPL + debug + session select|§C,V4,V6,V7,V8,V9,V10,V14
T23-29|x|Telegram channel: poller, send+typing, offset persistence, chat→session routing, backoff; civetweb status page|V2,V11,V25,§I
T30-48|x|Tools + runtime: mquickjs, js_eval, js_define_tool, JS replay, heartbeat, cron, spawn_agent, db_query, sub-agent lifecycle, token estimation, graceful shutdown, systemd/sysvinit, error handling, provider fallback, system prompt, shell timeout, web_fetch|V2,V3,V5,V10,V13,V15,V40,V41,V42
T49-72|x|Phase A tests + concurrency: context/workspace/wrapping tests, live integration tests, session state machine (CAS, turn tagging, incomplete interception, janitor, anti-crash, inbox primitives, atomic move, sub-agent completion, verification), CLI/Telegram/cron triggers, web console|V1,V7,V8,V12,V16,V17,V18,V19
T74-94|x|Multi-agent + daemon: agent discovery, config/prompt/skill loaders, tool whitelist, session↔agent binding, daemon epoll loop, signal pipe, agent process entry, response delivery, last_route, child tracking, spawn queue, daemon tests, context error skipping, LLM retry loop, graceful child shutdown, crash recovery, startup recovery|V20,V21,V22,V23,V24,V25,V26,V27,V28,V29,V30,V31,V32,V34
T95-104|x|StopReason normalization + shell sandbox + JS network: enum, map_stop_reason, entry storage, context V36 filtering, tests, namespace sandbox (CLONE_NEW*), http_fetch JS binding, allowed_hosts config|V35,V36,V37,V38
T105-118|x|Performance + streaming + heartbeat + policy: tool truncation, context_plan, RequestStreamer (CURLOPT_READFUNCTION), heartbeat trigger/suppression, [NO_REPLY], planning-only retry, CLI progress, HttpPolicy layer, JS sanitize, write-time truncation|V40,V41,V42,V44,V45,V46
T119-151|x|Agents table, memory, prompt assembly, cache hints, entry stats, mock test harness + integration tests (agent loop, retry, overflow, Telegram, daemon fork), shell hardening, mjs binary, template embedding, Telegram admin (auth, commands, /key, /config, /whitelist), approvals table + flow + delivery + callback + agent creation|V47,V48,V52,V53,V54,§D
T152-185|x|Memory blocks + DB optimization + compaction + split-column schema + kv config + secrets + integration tests (wire emission, large session, db_query filter, empty response, plan retry, tool loop, sub-agent, compaction)|V52,V55,V56,V57,V58,V59,V60,V61
T186-207|x|3-DB architecture: ephemeral agents, permission model, daemon secrets, bootstrap flow, configure_provider/channel/create_agent tools, channel→agent binding, token rate limit, 3-DB schema + init fns, agent_config table, exit code protocol, agent-only-own-DB, daemon inbox writes, session state, daemon reap dispatch, spawn_queue + approvals + cron in cclaw.db, CLI standalone, integration tests|V62,V65,V66,V67,V68,V69,V71,V72,V73,V74,V76,V77,V78,V79,V80,V81,V85
T208-224|x|Shell security: namespace sandbox, libcclaw_net.so, proxy thread, secret env injection, output masking, tests (proxy, denied hosts, filesystem isolation), log collector, test audits (unit, integration, e2e), CLI zero-config, partial-turn resume|V75,V82,V83,V86,V87
T225-237|x|Refinements: compaction refactor (token-based), mock server JSON templates, CWD read-only path, yolo mode, agent self-rename, zero-usage retry, get_response_text, CLI journal parity, log level system, trace logging, error classification, request_stream JSON fix|V91,V92,V93,V94,V95,V96,V97,B2
T238-253|x|Channel process model: channels/channel_events/channel_outbox/channel_state tables, daemon_wake FIFO, channel API library, daemon launcher + events consumer + outbox writer, Telegram→channel process refactor (getUpdates + outbox delivery), graceful shutdown, configure_channel update, tests|V98,V99,V100,V101,V103,V104,V105,V106,V107,V108
T254-270|x|Extensions + features: discovery, loader, cclaw API object, registerHook, hook dispatch (beforeRequest, beforeToolCall, afterToolCall, turnStart/End, afterResponse), callTool re-entrancy, integration into startup, tests, cost tracking, auto-recall FTS5, CLI streaming|V109,V110,V111,V112,V113,V114,V116,§D,V7
T271-282|x|CLI UX + agent management: agent/session pickers, default_agent kv, request_config tool, initial tool set enforcement, CWD rw bind-mount, agent creation defaults, sub-agent privilege reduction, AGENT_DEFAULT_* constants, absent-key semantics, named spawn fix|V117,V118,V119,V120,V122,V123,V124,B4
T277|~|[hold] agent template system — `extensions/agents/*.json`; schema: `{name, system_prompt, tools[], allowed_hosts[], memory_blocks[], ephemeral?}`; CLI "new agent..." offers templates + blank|§F
T283|x|e2e: streaming + thinking + tool calls — yolo, DeepSeek V4 Flash, verify thinking blocks handled, tool dispatch works, no ASAN/UBSan, exit 0; see [TEST-CLI.md](TEST-CLI.md)|B8,B9,B10
T284|x|e2e: non-streaming mode — `CCLAW_STREAM=0`, verify `"stream":false` in request, response printed at end; see [TEST-CLI.md](TEST-CLI.md)|§C
T285|x|e2e: multi-tool chain — file_write → shell_exec (python3) → verify output; see [TEST-CLI.md](TEST-CLI.md)|V10
T286|x|e2e: js_eval — Fibonacci via QuickJS, verify result correct, no crash; see [TEST-CLI.md](TEST-CLI.md)|V5
T287|x|e2e: web_fetch — fetch httpbin.org, verify content extraction; see [TEST-CLI.md](TEST-CLI.md)|V46
T288|.|e2e: pure reasoning (no tools) — thinking + content, stop_reason=stop, no tool calls; see [TEST-CLI.md](TEST-CLI.md)|V35

Test tiers (Makefile targets):
- `make test` — unit tests (no network, no LLM, fast, always run)
- `make test-integration` — mock-server tests (civetweb in-process, no external deps, ~seconds)
- `make test-e2e` — live LLM tests (require API key, hit real endpoints, skip if key missing)

## §B BUGS
id|date|cause|fix
B1|2026-05-31|GMICloud returns HTTP 200 w/ 0 tokens + null content + `finish_reason:"stop"` after tool-result follow-up; agent stored empty string as final response; `print_response` printed blank line|V94,V95 — `get_response_text` skips empty; T231 adds retry
B2|2026-06-01|`build_tools_fragment` closes JSON object w/ `}` — `max_tokens` field ⊥ included when tools present; every agentic request sent without output limit|T237
B3|2026-06-02|T268 added `cost_nano` column to INSERT SQL + schema template but pre-existing agent.db lacked column; `entry_append_with_turn` failed silently (prepare error → -1); agent returned 0 (success) w/ no response written; also: cost parser looked for `usage.total_cost` but OpenRouter sends `usage.cost`|fix: `db_open_agent` runs `ALTER TABLE entries ADD COLUMN cost_nano INTEGER` (idempotent); parser checks `cost` then `total_cost`; moved `test_integration_cli` → e2e tier (requires live LLM)
B4|2026-06-03|`process_spawn_queue` only supports unnamed spawn (child session in parent DB); named spawn (launch different agent by name) ⊥ implemented; also no parent ceiling enforcement on named spawn (V123 mode 2)|V123,T282
B5|2026-06-03|`auth_hdr[512]` stack buffer in `llm_call_with_fallback_stream` truncates API keys > 489 chars via snprintf; JWT/OAuth tokens (Vertex AI, Azure AD) silently truncated → persistent 401; no runtime detection|V125
B6|2026-06-03|`llm_call_with_fallback_stream` passes `resp` to fallback providers without `http_response_free(resp)` first; `http_post_stream` memsets resp to 0 → leaks previous allocation every fallback attempt|V126
B7|2026-06-03|outer `MAX_LLM_RETRIES` loop in `agent_run` re-catches 5xx errors already exhausted by inner retry+fallback chain; `continue` resets backoff → up to 15 requests (3×5) against provider that already refused 5×|V127
B8|2026-06-03|`setrlimit(RLIMIT_AS, 256MB)` blocks ASAN shadow memory (~20TB virtual) → `ERROR: Failed to mmap` crash before LLM call|V23 — skip RLIMIT_AS under ASAN
B9|2026-06-03|`free(tool_whitelist)` frees static `DEFAULT_TOOLS` global when `CCLAW_TOOLS` env unset → ASAN alloc-dealloc-mismatch abort after first turn|V119 — conditional free
B10|2026-06-03|`dest + (w < cap ? w : 0)` in `emit_entry_openai` → UB null+0 during size-calc pass (dest=NULL)|V60 — DEST_AT macro
B11|2026-06-03|Ctrl-C in CLI doesn't stop child agent — child in same pgroup receives SIGINT but continues streaming; parent blocked in fgets on stderr pipe|fix: child `setpgid(0,0)`, parent kills child on shutdown_requested

## §F FUTURE
- ~~CLI streaming response~~: implemented (T270) — `"stream":true` in CLI mode, SSE parsing, real-time token output to stdout; configurable via `CCLAW_STREAM` env var; daemon mode remains non-streaming
- Web chat: civetweb serves chat UI (SSE streaming for partial responses, session select/create, message history); block streaming to browser as assistant generates; replaces status-only page
- Intra-turn steering: agent checks inbox between tool executions, injects new messages into context mid-loop (Pi model: steering = interrupt, follow-up = queue until stop); complicates V18 atomic consumption — design carefully
- Session curation: ~~mid-session compaction~~ (now §T T160-T163); prune failed tool-call loops, automated "curation agent" that cleans up long-running sessions
- Session recreation: compose new sessions from cherry-picked existing entries (join table `curated_branches(session_id, entry_id, position)`) — avoids copying, entries stay immutable, new summary entries fill gaps
- Curation agent: background sub-agent that operates on another session's branch — identifies noise (repeated failures, dead-end tool calls), summarizes or removes them, produces a cleaner branch for continued work
- Telegram/non-CLI sessions can't easily branch interactively — curation agent fills that gap
- Seccomp-bpf: defense-in-depth syscall filtering for agent processes (block fork/execve/ptrace/mount — force all execution through shell_exec tool); both platforms have `CONFIG_SECCOMP_FILTER=y`; needs per-arch filter defs (ARM64 vs ARMv5) or `libseccomp`; add once daemon model stable and syscall needs empirically known
- ~~Filtered shell network~~: replaced by namespace sandbox (V82) + UDS proxy; shell children have no direct network; proxy enforces host allowlist
- Multi-arch releases: GitHub Actions matrix with Docker containers (arm64, armhf, amd64) producing tarballs; binary + "install libcurl" README; not Zig-style cross-compile — native build per target with system libcurl; ARMv5TE (Pogoplug) likely needs dedicated Debian armel container
- ~~Cost tracking~~: implemented (T268) — `cost_nano` column, `session_cost()`, CLI print on exit; future: per-model rate table for direct providers, web dashboard, per-agent aggregation
- MCP extension: reference JS extension that speaks Model Context Protocol over stdio/HTTP; discovers remote tools, registers them via the extension API; ships as a built-in example extension (depends on T254-T263)
- OpenAI Device Code auth: OAuth 2.0 Device Authorization Grant (RFC 8628) for ChatGPT/Codex subscription access without API key; show URL + code → user approves on phone/laptop → poll for token; works headless (no browser callback); store access+refresh tokens in DB; auto-refresh on expiry; identify as `cclaw/<version>` User-Agent
- System prompt in DB: move system prompts from `agents/<name>/system.md` to a `prompts` table (id, name, template TEXT, created_at); template vars `{session_id}`, `{date}`, `{agent_name}`; agents reference prompt by name/id in config; allows runtime editing without filesystem access; keep file-based loading as fallback/import path
- ~~Workspace model refinement~~: implemented — `.cclaw/agents/<name>/workspace/` per agent; `.cclaw/agents/<name>/agent.db` per agent; shell children namespace-restricted to workspace only (V82, V62)
- ~~Auto-recall (FTS5)~~: implemented (T269) — cross-session FTS5 search, injected as system message via request streamer; future: configurable threshold, memory_blocks search
- Vector recall (NEXT): embedding-based semantic search over entries + memory_blocks + workspace files; hybrid w/ FTS5 (RRF merge); requires embedding model (local or API); `passages` table (text, embedding BLOB, source_type, source_id); agent tool `memory_search(query)` for explicit recall; auto-recall injects top hits same as FTS5 path
- Input logprobs pruning: use prompt token logprobs to intelligently prune history — low-probability tokens = surprising/informative (keep), high-probability = redundant (safe to drop); requires local model inference (vLLM `--return-prompt-logprobs` or llama.cpp); not available via hosted APIs
- Shell command safety gate: fast LLM (or pattern-matching) check on `shell_exec` command string before execution; detect: secret values literally in command, `env`/`printenv` dumps, exfiltration patterns (`curl $SECRET` to non-allowlisted host), encoded secret leaks; runs at the command boundary (after LLM produces command, before shell child launches); lightweight — small model or regex heuristics; false positives → block + ask LLM to reformulate
- Agent templates: `extensions/agents/*.json` ship declarative agent specs (name, system_prompt, tools[], allowed_hosts[], memory_blocks[], ephemeral flag); CLI "new agent..." surfaces installed templates; template instantiation creates real agent in cclaw.db + agent.db; built-in templates: "default" (minimal tools), "ephemeral" (no memory persistence), "developer" (shell + web_fetch + full hosts)
