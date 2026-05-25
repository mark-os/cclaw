# SPEC

## §G GOAL
minimal autonomous AI agent in C, inspired by Pi & OpenClaw — multi-channel (CLI, Telegram), SQLite backbone, MicroQuickJS runtime tools, sub-agents

## §C CONSTRAINTS
- C11, `-Wall -Wextra -Werror`
- blocking I/O, threads over callbacks, no event loops
- SQLite WAL mode — sole persistence layer
- system libcurl (dynamic link); vendor cJSON, sqlite3, civetweb, mquickjs
- primary target: EC2 t4g.small ARM64 AL2023
- single user (Mark) — no multi-tenant auth
- OpenAI-compatible chat completions format ∀ providers
- default provider: OpenRouter → DeepSeek V4 Flash (`deepseek/deepseek-v4-flash`)
- no streaming — full response only (simplifies agent loop, tool parsing)
- daemon+fork model: daemon schedules, forks agent processes, reaps; never executes LLM logic
- agent process = one session turn (drain inbox → LLM loop until stop → write response → exit)
- IPC: SQLite sole message store; pipe/eventfd wakeup signal only (no message broker)
- landlock per-agent process (restrict fs to workspace + read-only system paths)

## §I INTERFACES
- cmd: `./build/cclaw --cli` → stdin/stdout REPL, creates/resumes session in shared DB
- cmd: `./build/cclaw --cli --debug` → raw LLM req/resp JSON to stderr
- cmd: `./build/cclaw` → daemon mode (epoll loop: reap children, wake on signal pipe, fork agents; Telegram poller thread + civetweb status page run in-process)
- cmd: `./build/cclaw config.json` → explicit config file
- cmd: `./build/cclaw --sub-agent --session-id=X --task="..."` → sub-agent process
- env: `OPENROUTER_API_KEY` required (minimum to run)
- env: `CCLAW_PROVIDER`, `CCLAW_MODEL`, `CCLAW_TELEGRAM_TOKEN`, `CCLAW_DB_PATH`, `CCLAW_WEB_PORT`
- file: `config.json` — provider, model, tokens, channels, workspace, db_path, admin_chat_ids[]
- file: `agents/<name>/agent.json` — per-agent: model, workspace, tools, max_iterations, allowed_hosts, memory_blocks[]
- file: `agents/<name>/system.md` — system prompt template
- file: `agents/<name>/skills/*.md` — skill instructions injected into system prompt
- api: Telegram Bot API (long-poll `getUpdates`, `sendMessage`, `sendChatAction`)
- api: OpenAI-compatible `POST /chat/completions` (any provider)
- web: `GET /` → minimal status page (active sessions, state metrics, inbox depths, uptime)
- tool: `shell_exec` — run cmd, return stdout/stderr
- tool: `file_read` — read file (workspace-restricted)
- tool: `file_write` — write file (workspace-restricted)
- tool: `js_eval` — execute JS in sandboxed mquickjs
- tool: `js_define_tool` — register JS fn as callable tool (session-persistent)
- js binding: `http_fetch(url, {method, headers, body, sanitize})` — C-provided, enforces agent `allowed_hosts` + SSRF protection; `sanitize: true` strips HTML + homoglyphs + boundary wraps (same as web_fetch); sole network path from JS runtime
- tool: `spawn_agent` — fork sub-agent process (accepts `background` param, default blocking)
- tool: `db_query` — execute read-only SQL against cclaw.db (SELECT only, no mutations)
- tool: `web_fetch` — HTTP GET URL, extract text from HTML, external input protection wrapper
- tool: `memory_create` — create a new memory block (label, description, value?); agent-initiated; default char_limit 5000
- tool: `memory_append` — append text to a memory block by label; respects char_limit + read_only
- tool: `memory_replace` — find-and-replace within a memory block by label; respects char_limit + read_only
- tool: `approval_request` — propose config/permission change for admin approval; types: whitelist_host, create_agent, model_change, tool_enable; agent blocks until admin responds
- db: `cclaw.db` — SQLite 3.53, WAL, FTS5, JSON functions

## §D DATA
entries table: `id`, `session_id`, `parent_id`, `turn_id`, `created_at`, `data TEXT NOT NULL` (JSON), generated columns: `type`, `role`

Entry `data` format (discriminated by `$.type`):
- `{"type":"message","role":"user","content":"..."}`
- `{"type":"message","role":"assistant","content":[{"type":"text","text":"..."},{"type":"tool_call","id":"...","name":"...","arguments":{...}}],"model":"...","usage":{"input":N,"output":N},"stop_reason":"<StopReason>"}`

StopReason enum (normalized, provider-agnostic — Pi model):
| Value | Meaning | Provider `finish_reason` sources |
|-------|---------|----------------------------------|
| `stop` | normal completion | `"stop"`, `"end"`, `"end_turn"`, `null` |
| `length` | hit max tokens | `"length"`, `"max_tokens"` |
| `tool_use` | wants tool calls | `"tool_calls"`, `"function_call"`, `"tool_use"` |
| `error` | provider error, content_filter, parse failure, missing finish_reason | `"content_filter"`, `"network_error"`, unknown, missing |
| `aborted` | client-side abort (SIGTERM, user cancel, timeout) | (internal — not from provider) |
- `{"type":"message","role":"tool_result","tool_call_id":"...","name":"...","content":"...","is_error":false}`
- `{"type":"message","role":"system","content":"..."}`
- `{"type":"compaction","summary":"...","first_kept_id":N}`

inbox table: `id`, `session_id`, `source TEXT`, `payload TEXT`, `created_at`, `consumed INTEGER DEFAULT 0`

sessions table: `id`, `name`, `leaf_id`, `agent_name TEXT`, `state` (idle|running|waiting), `error_count`, `last_route TEXT`, `created_at`, `updated_at`

Agent config on disk (`agents/<name>/`):
- `agent.json` — model override, tool whitelist, max_iterations, workspace path, allowed_hosts
- `skills/` — per-agent skill files (markdown, injected into system prompt)

Agent state in DB (replaces filesystem-based MEMORY.md, SOUL.md, system.md, HEARTBEAT.md):

agents table: `id`, `name TEXT UNIQUE`, `config TEXT` (JSON — model, tools, limits, allowed_hosts), `system_prompt TEXT` (template, supports `{session_id}`, `{date}`, `{agent_name}`), `heartbeat TEXT` (proactive task instructions, read on heartbeat turns), `created_at`, `updated_at`

approvals table: `id`, `session_id`, `agent_name TEXT`, `type TEXT` (whitelist_host|create_agent|model_change|tool_enable), `payload TEXT` (JSON — request details), `status TEXT DEFAULT 'pending'` (pending|approved|denied), `admin_chat_id`, `created_at`, `resolved_at`

memory_blocks table: `id`, `agent_name TEXT NOT NULL`, `label TEXT NOT NULL`, `value TEXT DEFAULT ''`, `description TEXT` (tells agent what block is for), `char_limit INTEGER DEFAULT 5000`, `read_only INTEGER DEFAULT 0`, `created_at`, `updated_at`; UNIQUE(agent_name, label); default: zero blocks — agent creates its own via `memory_create`; optionally pre-seeded from `agent.json` `memory_blocks[]` on first reference

Agent config `memory_blocks` field (in `agent.json`):
```json
"memory_blocks": [
  {"label": "persona", "description": "Agent identity and tone", "value": "I am...", "char_limit": 5000, "read_only": false},
  {"label": "human", "description": "Known facts about the user", "char_limit": 5000, "read_only": false},
  {"label": "instructions", "description": "Standing orders", "value": "Always...", "read_only": true}
]
```
`read_only: true` → block visible in context but agent memory tools reject edits; `read_only: false` (default) → agent can append/replace via tools

Design:
- Agent identity (prompts, memory blocks) lives in SQLite → portable by copying DB, no filesystem sync
- Agent can self-modify memory blocks via tools (`memory_append`, `memory_replace`) — just DB writes; `description` field guides agent on block purpose (visible in context, not editable by agent)
- System prompt assembled at turn start: `system_prompt` template + memory blocks (rendered w/ label, description, metadata, value) + skills
- `agents/<name>/agent.json` on disk is the bootstrap/import path — loaded once to seed the DB row; DB is authoritative after that
- Workspace is just a working directory for file tools — no magic filenames, no special semantics
- `/tmp/cclaw-<session_id>/` — ephemeral per-session scratch (tool output overflow)
- Writable paths under landlock/namespace: workspace + `/tmp/cclaw-<session_id>/` + DB file

## §V INVARIANTS
V1: ∀ tool exec (`file_read`, `file_write`) → path ! ∈ workspace dir for that agent
V2: ∀ LLM call returning 429 → retry w/ exponential backoff, respect `Retry-After`
V3: ∀ sub-agent → max depth 2, max 3 concurrent/parent, max 10 system-wide
V4: ∀ DB access → WAL mode, `busy_timeout` ≥ 5000ms
V5: ∀ `js_eval` → 1MB heap cap, 10M instruction limit
V6: ∀ agent turn → per-turn arena (512KB scratch), destroyed after turn
V7: ∀ context build → load ≤ `max_history_tokens` (default 60% of model context window) most recent turns; prepend cutoff notice; older messages searchable via FTS5
V8: ∀ context build → never cut mid-tool-call; cut at valid turn boundary (before user msg | after complete assistant response)
V9: ∀ LLM request → omit `"tools"` field entirely when no tools registered (never send `"tools": []`)
V10: ∀ tool crash/timeout → produce error result, continue agent loop (never crash loop)
V11: ∀ Telegram msg → chunk at 4096 chars, split at paragraph then sentence boundaries; sleep ≥ 3s between chunks to same chat_id (proactive rate limit — Telegram allows 20 msg/min/chat); backoff (T27) is fallback, not primary flow control
V12: workspace ! per-agent (config `workspace` field per agent, fallback to `./workspace/{agent_name}`)
V13: sub-agent spawn → blocking: parent writes assistant entry (w/ tool_call) + spawn request, exits (state→"waiting"); sub-agent result arrives in parent inbox as tool_result; daemon forks parent on completion. background: parent continues, sub-agent posts to parent inbox on completion. Exception: CLI mode uses fork+waitpid in-process (V39)
V14: session tree structure → entries w/ `id` + `parent_id` (Pi model); branching structure in DB even if branching UI deferred
V15: ∀ tool result → optionally wrap in `<tool_result name="X">...</tool_result>` tags to sanitize external data (configurable per tool)
V16: ∀ agent_run → session state machine (idle|running|waiting) is the lock; daemon is sole fork-parent → no concurrent agents per session possible → CAS unnecessary; `lock_holder`/`lock_acquired_at` columns removed
V17: ∀ turn → entries share a `turn_id`; incomplete turns intercepted: if session.state == "waiting" & last tool_call is `spawn_agent` → resume (not failure); else synthesize failure notice via `context_build`
V18: ∀ inbox message → consumed exactly once into session entries via single atomic SQLite transaction (`BEGIN EXCLUSIVE`)
V19: ∀ session state transition → executed via strict atomic UPDATE with WHERE clauses targeting expected states to prevent TOCTOU
V20: ∀ session → `agent_name` identifies agent; config loaded from `agents/<name>/` on disk; fallback to global config when NULL
V21: ∀ agent execution → daemon forks dedicated process; daemon ⊥ executes LLM logic
V22: ∀ agent process → landlock applied at fork: write restricted to agent workspace + `/tmp/cclaw-<session_id>/`; read-only `/usr`, `/etc`, curl CA bundle; graceful fallback if landlock unavailable (Pogoplug kernel 6.19.9 has `CONFIG_SECURITY_LANDLOCK=n` — log warning, continue without)
V23: ∀ agent process → `setrlimit` at fork: RLIMIT_AS (256MB default), RLIMIT_CPU (300s default), RLIMIT_NOFILE (64)
V24: ∀ session → at most 1 active agent process; daemon forks only when state == "idle"; "running" and "waiting" block new forks
V25: ∀ channel inserter (Telegram, cron, sub-agent) → inbox_insert + write session_id to signal pipe; ⊥ run agent logic
V26: ∀ agent process exit → daemon reaps via `waitpid`, reads `last_route` from session, delivers response to channel
V27: ∀ inbox consumption → agent updates `last_route` from newest item's `source` field
V28: ∀ context_build → skip assistant entries w/ `stop_reason` ∈ {"error","aborted"} (per V36); their tool_calls excluded from pending tracking (no synthetic tool_results for dead calls)
V29: ∀ LLM response → missing `finish_reason` (truncated stream, socket broken) → treat as error, write entry w/ `stop_reason: "error"` + partial content preserved, retry per V2
V30: ∀ agent process crash (SIGKILL, OOM, RLIMIT_CPU) → no entry written; daemon reaps; next fork for session → V17 detects incomplete turn, synthesizes failure notice
V31: ∀ daemon shutdown (SIGTERM) → forward signal to active children; children write error entry if possible; unfinished turns recovered via V17 on next startup
V32: ∀ LLM error retry → `continue` semantics: re-send last valid context state (after V28 stripping); max 3 retries per turn before writing final error entry + exiting
V33: ∀ blocking sub-agent completion → sub-agent writes tool_result to parent inbox (keyed by `tool_call_id`) → daemon transitions parent state "waiting"→"idle" → daemon forks parent
V34: ∀ agent process → `prctl(PR_SET_PDEATHSIG, SIGTERM)` after fork; daemon death auto-kills children; daemon startup recovery: reset all "running"→"idle" (children already dead); "waiting" w/ result in inbox → "idle"; "waiting" w/o result → write error tool_result to inbox, "idle"
V35: ∀ LLM response → normalize provider `finish_reason` → `StopReason` enum (`stop|length|tool_use|error|aborted`) before storing entry; `map_stop_reason()` in `llm.c` is sole normalization point
V36: ∀ context_build → skip assistant entries w/ `stop_reason ∈ {error, aborted}`; synthesize error tool_results for their orphaned tool_calls (`"error: process terminated during execution"`); never send errored/aborted turns to LLM
V37: ∀ `shell_exec` child → `unshare(CLONE_NEWUSER | CLONE_NEWNET | CLONE_NEWNS)` before exec; remount `/` read-only, bind-mount workspace read-write; no network by default; per-agent `"shell_network": true` skips `CLONE_NEWNET`; graceful fallback if `unshare()` fails (log warning, run unsandboxed)
V38: ∀ JS runtime (`js_eval`, `js_define_tool`) → C-provided `http_fetch(url, opts)` binding is sole network path; binding enforces per-agent `allowed_hosts` allowlist + SSRF protection (reject private IPs) before calling libcurl; no allowlist = no network from JS
V39: ∀ `spawn_agent` in CLI mode → fork+exec+waitpid in-process (blocking) or fork+continue (background); CLI has no daemon — ⊥ use "exit into waiting state" pattern; tool_subagent must detect execution mode and branch accordingly
V40: ∀ tool_result content > 50KB or 2000 lines → truncate before storing in DB entry; full output written to `/tmp/cclaw-<session_id>/<tool_call_id>.out`; truncated result gets suffix `[truncated — showing last {N} lines of {M}. Full output: /tmp/cclaw-<session_id>/<tool_call_id>.out]`; agent can `file_read` the full output path if needed; temp dir cleaned on session idle timeout or daemon restart
V41: ∀ LLM request → built via `CURLOPT_READFUNCTION` streaming from SQLite cursor; ⊥ load full session into memory; two-pass: (1) plan entry IDs + cut point, (2) stream JSON from cursor; per-agent memory footprint ≤ arena + curl buffers (~2-5MB), not session-proportional
V42: ∀ heartbeat → daemon triggers agent run w/ heartbeat prompt; agent reads `HEARTBEAT.md` if present, acts on tasks; response `HEARTBEAT_OK` = sentinel (suppressed, ⊥ delivered to channel); any other response → deliver to channel via `last_route`
V43: ∀ tool dispatch → track last N calls (name + args hash + result hash); if same call repeated ≥ 5× w/ no progress → inject warning into tool_result; ≥ 10× → force-stop agent loop w/ error ("tool loop detected")
V44: ∀ Telegram group msg → if agent response contains `[NO_REPLY]` → suppress delivery (⊥ send to chat); agent decides relevance per system prompt guidance
V45: ∀ agent response → if `stop_reason == stop` & no tool_calls & response is plan-only (bullet list + "I'll do X" promise, no tool action taken) → re-prompt once: "Do not restate the plan. Act now: take the first concrete tool action. If blocked, state the blocker in one sentence."
V46: ∀ outbound HTTP (libcurl) → `http_policy` layer validates hostname before connect: (1) check deny list (blocked_hosts[]), (2) check allow list (allowed_hosts[] — empty = allow all for LLM/telegram, deny all for agent tools), (3) block RFC1918/loopback/link-local IPs (SSRF); policy configured per-caller: LLM+telegram = unrestricted (trusted endpoints), web_fetch+js_http_fetch = agent's allowed_hosts + SSRF block; `web_fetch` always sanitizes (strip HTML + homoglyph + boundary wrap); JS `http_fetch` accepts `{sanitize: true}` option for same treatment
V47: ∀ `shell_exec` child → PATH restricted to `/bin:/usr/bin`; unset `OPENROUTER_API_KEY`, `GEMINI_API_KEY`, `HOME`, and all `CCLAW_*` env vars before exec; prevents agent from invoking cclaw binary or leaking credentials via shell
V48: ∀ `shell_exec` child → `mjs` binary available at fixed path (e.g. `/usr/local/lib/cclaw/mjs`); PATH ! include its directory — agent invokes via absolute path; `mjs` = standalone mquickjs evaluator w/ `fetch()` binding only
V49: ∀ `mjs` process (spawned via shell_exec) → inherits socketpair fd from agent process (pre-fork); `fetch()` binding serializes request over inherited fd → agent reads, checks `allowed_hosts` allowlist + SSRF policy (V46), performs libcurl call, writes response back; mjs has zero direct network capability; fd number passed via env var `MJS_FETCH_FD`
V50: ∀ `mjs` fetch proxy protocol → request: `<4B len big-endian><JSON: {"url","method","headers","body"}>`, response: `<4B len big-endian><JSON: {"status","headers","body","error"}>` over inherited fd; agent-side enforces timeout (30s default) per fetch call; on policy deny → respond w/ `{"status":403,"error":"host not in allowed_hosts"}`
V51: ∀ `mjs` invocation via shell → composable w/ unix pipes; stdout/stderr flow through shell pipeline normally; fetch fd is side-channel (⊥ interfere w/ stdio); agent can: `mjs -e 'await fetch(...)' | jq .items[] | grep error`
V52: ∀ API key entered via Telegram admin dialog → written directly to config file on disk; ⊥ stored in session entries, inbox, or any DB field; ⊥ appears in LLM context; Telegram poller handles key input before inbox_insert (bypasses agent entirely)
V53: ∀ Telegram admin command (config, key, whitelist) → restricted to `admin_chat_ids[]` in config; unauthorized chat_id → silent ignore (⊥ error msg, ⊥ inbox_insert)
V54: ∀ config/permission mutation → requires admin approval; agent may propose changes via `approval_request` tool (writes to `approvals` table); daemon delivers proposal to admin as inline keyboard; admin approve → daemon applies change + posts confirmation to agent inbox; admin deny → posts denial to agent inbox; agent ⊥ mutates config/permissions directly
V55: ∀ memory block edit → agent tools modify `value` only; `label`, `description`, `char_limit`, `read_only` immutable from agent; `value` length ! ≤ `char_limit`; blocks persist across sessions (agent-scoped, not session-scoped)

## §T TASKS
id|status|task|cites
T1|x|Makefile — minimal, grows w/ modules|§C
T2|x|arena allocator (`arena.c`) — create, alloc, destroy|V6
T3|x|core types (`types.h`) — Message, Session, Entry, Config structs|V14
T4|x|config (`config.c`) — parse JSON + env var overrides|§I.file,§I.env
T5|x|DB init (`db.c`) — open, create tables, WAL mode, pragmas|V4
T6|x|session CRUD — create, list, get_branch (leaf→root), set_leaf|V14
T7|x|entry append + tree ops (parent_id linking)|V14
T8|x|FTS5 setup + search fn over message content|V7
T9|x|HTTP wrapper (`http.c`) — POST w/ headers, response buffer via libcurl|§C
T10|x|LLM request builder (`llm.c`) — messages + tools → JSON|V9
T11|x|LLM response parser — extract content, tool_calls, usage|§I.api
T12|x|context window manager — select turns ≤ budget, cutoff notice, valid boundaries|V7,V8
T13|x|agent loop (`agent.c`) — call LLM, dispatch tools, repeat until done|V10
T14|x|tool registry — register/lookup by name, schema storage|§I.tool
T15|x|max iterations guard|§I.file
T16|x|session integration — load branch, append entries, flush to DB|V14,T6
T17|x|`shell_exec` tool — popen, capture stdout+stderr, timeout|§I.tool
T18|x|`file_read` tool — workspace path restriction|V1
T19|x|`file_write` tool — workspace path restriction|V1
T20|x|CLI REPL (`cli.c`) — read line, send to agent, print response|§I.cmd
T21|x|CLI debug mode — raw req/resp JSON to stderr|§I.cmd
T22|x|CLI session selection (create new / resume existing)|§I.cmd
T23|x|Telegram poller (`telegram.c`) — getUpdates loop in thread, inbox_insert + signal daemon pipe|§I.api,V25
T24|x|Telegram send + typing indicator (every 4s while working)|V11
T25|x|Telegram offset persistence in DB (survives restart)|§I.api
T26|x|Telegram chat_id → session routing|§I.api
T27|x|Telegram exponential backoff on transient errors|V2
T28|x|civetweb integration — start server, register routes|§I.web
T29|x|status page — active sessions, uptime, sub-agent status|§I.web
T30|x|vendor mquickjs, integrate into build|§C
T31|x|`js_eval` tool — execute code, return result|V5
T32|x|`js_define_tool` — register JS fn as tool (session-persistent)|§I.tool
T33|x|JS context replay on session reload|T32
T34|x|heartbeat timer thread + system msg injection|§C
T35|x|cron table + scheduler thread|§I.db
T36|x|`cron_set`/`cron_list`/`cron_remove` tools|T35
T37|x|`spawn_agent` tool — post spawn request to dispatch queue; daemon forks sub-agent|V3,V13,V21
T38|x|`db_query` tool — read-only SQL (SELECT only, reject mutations)|§I.tool
T39|x|sub-agent lifecycle (limits, cleanup, crash isolation)|V3
T40|x|token estimation (chars/4 heuristic)|V7
T41|x|graceful shutdown (SIGINT/SIGTERM)|§C
T42|x|systemd unit file (`cclaw.service`) — restart on failure, env file, journal logging (no test)|§C
T43|x|SysVinit init script — network wait, respawn loop, PID mgmt, stale PID detect (no test)|§C
T44|x|error handling — 429 retry, context overflow detect, JSON parse failure recovery|V2,V10
T45|x|provider fallback chain — config array, try next on 5xx/timeout (test: Gemini `gemma-4-31b-it` via `GEMINI_API_KEY`)|V2
T46|x|system prompt — load from config per agent, template vars `{session_id}`, `{date}`|§I.file
T47|x|`shell_exec` timeout — SIGKILL child after configurable N seconds (default 30)|V10
T48|x|`web_fetch` tool — HTTP GET, extract text from HTML, wrap output in external input protection|V15
T49|x|test: context window cuts at valid turn boundary, never mid-tool-call|V7,V8
T50|x|test: per-agent workspace isolation (file tools reject paths outside workspace)|V1,V12
T51|x|test: external content wrapping — boundary markers, homoglyph sanitization|V15
T52|x|integration test: live OpenRouter call → parse response, verify tool_calls round-trip|§I.api
T53|x|integration test: agent loop end-to-end (prompt → tool call → tool result → final answer)|V10,§I.api
T54|x|integration test: provider fallback (kill primary, verify fallback fires)|T45
T55|x|DB Schema: sessions CAS columns (state, lock_holder, lock_acquired_at, error_count) + inbox table|V16,V18
T56|x|CAS Acquire/Release (`session_try_acquire`)|V16,V19
T57|x|agent.c Turn Tagging (assign `turn_id` via index query)|V17
T58|x|context.c Incomplete Turn Interception (synthetic failure + notice)|V17
T59|x|Janitor Sweep Logic (stale locks, orphan pending recovery)|V16,V19
T60|x|Anti-Crash Loop Limit (`error_count` tracking, quarantine ≥ 3)|V19
T61|x|Phase A Integration (verification matrices for overlap rejection)|V16
T62|x|inbox Core Primitives (`inbox_insert`, `inbox_peek`)|V18
T63|x|Atomic Move Transaction (`inbox_consume_into_entries`)|V18
T64|x|Background sub-agent completion (post result to parent inbox)|V13,V18
T65|x|`spawn_agent` blocking mode (wait on child, return result as tool output)|V13,V3
T66|x|Verification Tests (atomic rollbacks mid-consumption)|V18
T67|x|CLI workspace triggers (acquire/release keep-alive framework)|V16
T68|x|Telegram intake handlers (inbox_insert + trigger local lock)|V16
T69|x|Cron process actions (transactional inbox wrappers)|V16
T70|x|Integration Test: parallel high-throughput network payloads|V16,V18
T71|x|Web console updates (state metrics, lock holders, backlog depths)|§I.web
T72|x|CLI terminal resume paths (echo unread inbox counts)|§I.cmd
T74|x|~~Bind runtime parameters (`stale_lock_timeout`) to config~~ superseded by daemon model (V16)|§I.file
T75|x|agent discovery — scan `agents/` dir, list available agents by name|V20
T76|x|agent config loader — read `agents/<name>/agent.json`, merge w/ global config|V20,V12
T77|x|system prompt loader — read `agents/<name>/system.md`, template vars `{session_id}`, `{date}`, `{agent_name}`|V20,T46
T78|x|per-agent tool whitelist — filter tool registry by agent config|V20,§I.tool
T79|x|session↔agent binding — session_create accepts agent_name, load config from disk at agent_run|V20
T80|x|skill loader — scan `agents/<name>/skills/*.md`, inject into system prompt|V20
T81|x|daemon main loop — epoll on signal_pipe + SIGCHLD self-pipe, fork/reap agents|V21,V24,V26
T82|x|signal pipe — create at daemon start, inserters write session_id to wake daemon|V25
T83|x|agent process entry — fork, landlock, setrlimit, drain inbox, run agent loop, exit|V21,V22,V23
T84|x|landlock setup — per-agent workspace write, system read-only, deny network (curl via inherited fd?)|V22
T85|x|response delivery — daemon reads `last_route`, dispatches to correct channel|V26
T86|x|`last_route` tracking — agent updates on inbox consumption from newest source|V27,§D
T87|x|daemon child tracking — map pid→session_id, enforce V24 (no dup fork)|V24
T88|x|daemon spawn queue — sub-agent spawn requests from agent processes, daemon picks up + forks|V21,T37
T89|x|test: daemon forks agent on inbox signal, reaps on exit, delivers response|V21,V26
T90|x|context_build: skip errored/aborted assistant entries + their orphaned tool_calls|V28
T91|x|LLM error retry loop — max 3 retries, re-send clean context (V28 stripped), write final error entry on exhaust|V29,V32
T92|x|graceful child shutdown — daemon SIGTERM → forward to children, children attempt error entry before exit|V31
T93|x|test: agent crash (simulated SIGKILL) → next fork recovers via V17 incomplete turn notice|V30
T94|x|daemon startup recovery — scan non-idle sessions, kill orphans, evaluate state, re-track or reset|V34
T95|x|`StopReason` enum in `types.h` — `STOP_REASON_STOP`, `LENGTH`, `TOOL_USE`, `ERROR`, `ABORTED`|V35
T96|x|`map_stop_reason()` in `llm.c` — normalize provider `finish_reason` string → `StopReason` enum|V35
T97|x|`Message.stop_reason` field — add to struct, populate from `LlmResponse.finish_reason` via T96|V35,§D
T98|x|`entry_append` stores `stop_reason` in entry `data` JSON; `session_get_branch` reads it back|V35,§D
T99|x|`context_build` V36 filtering — skip `error`/`aborted` assistant entries, synthesize orphaned tool_results|V36,V28
T100|x|test: StopReason normalization — all provider finish_reason variants map correctly|V35
T101|x|test: context_build skips errored entries, synthesizes tool_results for orphaned calls|V36
T102|x|`shell_exec` namespace sandbox — `unshare(NEWUSER\|NEWNET\|NEWNS)`, remount ro, workspace rw, fallback if unavailable|V37
T103|x|`http_fetch` JS binding — C function exposed to mquickjs; parse URL, check `allowed_hosts`, SSRF reject private IPs, call libcurl, return response|V38
T104|x|per-agent `allowed_hosts` config — array of hostnames in `agent.json`, loaded into agent config, passed to `http_fetch` binding|V38,§I
T105|x|tool result truncation in `context_build` — shared `truncate_result(buf, len)` util enforcing 50KB/2000 lines on tool_result messages before sending to LLM; full results stay in DB|V40
T106|x|streaming request planner — `context_plan()` returns ordered entry ID list + cut point + token budget from SQLite (pass 1, no content loaded)|V41,V7,V8
T107|x|`RequestStreamer` state machine — phases: preamble → entries (cursor step + reshape per-entry JSON) → tools → close; implements `CURLOPT_READFUNCTION` callback|V41
T108|x|integrate streaming request into `agent.c` — replace `llm_build_request` (full-buffer) with `RequestStreamer`; verify retry resets cursor|V41,V2
T109|x|heartbeat agent trigger — daemon injects heartbeat user msg into session inbox, forks agent; heartbeat prompt: "Read HEARTBEAT.md if present. Follow it. If nothing needs attention, reply HEARTBEAT_OK."|V42,T34
T110|x|`HEARTBEAT_OK` sentinel suppression — daemon checks final assistant response; if content == `HEARTBEAT_OK` → suppress delivery (⊥ send to channel); else deliver normally|V42,V26
T111|x|`HEARTBEAT.md` workspace file — optional; defines proactive tasks (reminders, checks, maintenance); agent reads via `file_read` during heartbeat turn|V42
T112|x|tool loop detection — hash(name+args) history ring buffer (last 30 calls per session); on dispatch, check streak; ≥ 5 same → inject warning in result; ≥ 10 → return error + break loop|V43
T113|x|`[NO_REPLY]` suppression — Telegram group delivery checks response for marker; if present, skip `sendMessage`; system prompt instructs agent when to use it|V44
T114|x|planning-only retry — after final assistant response w/ no tool_calls, detect plan-only pattern (bullets + promise verbs, no action); re-prompt once w/ act-now instruction; max 1 retry|V45
T115|x|CLI mid-turn progress — always-on: stream intermediate assistant text + tool call names/args as they execute; tool results truncated aggressively for display (shorter than V40 LLM limit); `--debug` adds raw JSON req/resp on top|§I.cmd
T116|x|`HttpPolicy` layer — struct w/ allowed_hosts[], blocked_hosts[], block_private flag; `http_check_policy(url, policy)` validates before curl; integrate into `http_get` (web_fetch), JS `http_fetch` binding; LLM/telegram calls pass NULL policy (unrestricted)|V46
T117|x|JS `http_fetch` sanitize option — `http_fetch(url, {sanitize: true})` applies html_strip_tags + sanitize_homoglyphs + boundary wrap (reuse web_fetch logic); default false (raw response)|V46,V38
T118|x|tool result write-time truncation — truncate at append time (not context_build); full output to `/tmp/cclaw-<session_id>/<tool_call_id>.out`; reference path in truncation notice; agent can `file_read` the path; update landlock to allow `/tmp/cclaw-*` write; clean temp dir on session idle or daemon restart|V40,V22
T119|x|`agents` table — schema: id, name, config(JSON), system_prompt, soul, memory, heartbeat, created_at, updated_at; seed from `agents/<name>/agent.json` + `system.md` on first reference; DB authoritative after seed|§D
T120|x|~~`soul_edit` tool~~ superseded by `memory_replace` on `persona` block|§I
T121|x|~~`memory_set` tool~~ superseded by `memory_append`/`memory_replace` on `human` block|§I
T122|x|system prompt assembly — at turn start: render template (system_prompt) + inject soul + inject memory + inject skills; replace current file-based `config_render_system_prompt`|§D,§I
T123|x|prompt cache hints — send provider-appropriate cache markers in LLM requests: Anthropic `cache_control` on system/last-tool/last-user; OpenAI `prompt_cache_key` + `prompt_cache_retention`; DeepSeek prefix caching (automatic but benefits from stable message ordering); configurable per-provider in config|§C
T124|x|entry stats columns — store `token_estimate INTEGER` (chars/4) on each entry at insert time; allows `context_plan` to sum tokens via index scan without loading JSON data; also store `content_bytes INTEGER` for quick size checks; avoids full-row reads during preflight planning pass (V41)|V41,§D
T125|x|mock LLM server test harness — civetweb on port 0 in-process; register `/v1/chat/completions` handler; configurable canned responses per test; helper: `mock_server_start()` → returns port, `mock_server_stop()`|§C
T126|x|integration test: agent loop with mock LLM — multi-turn tool call sequence (mock returns tool_call → agent dispatches → mock returns final); verify entries in DB match expected flow|V10,T125
T127|x|integration test: retry + backoff with mock — mock returns 429 with Retry-After, then 200; verify agent retries correctly and respects delay|V2,T125
T128|x|integration test: context overflow recovery — mock returns 400 with "context window" error; verify agent detects overflow|T125
T129|x|integration test: mock Telegram API — mock getUpdates + sendMessage endpoints; verify poll→inbox→agent→deliver cycle end-to-end|T125,V25
T130|x|integration test: daemon fork+reap with mock LLM — daemon forks agent, agent hits mock, writes response, daemon reaps and delivers|T125,V21
T131|x|`shell_exec` PATH + env hardening — set `PATH=/bin:/usr/bin` in child; unset API keys, HOME, CCLAW_* before exec; test: verify `env` output clean, verify cclaw binary unreachable|V47
T132|x|`mjs` standalone binary — build mquickjs evaluator (`vendor/mquickjs/`) as separate binary; accepts `-e 'code'` or filename arg; links no libcurl; `fetch()` binding reads/writes on fd from `MJS_FETCH_FD` env var; install to `/usr/local/lib/cclaw/mjs`|V48,V49,V51
T133|x|`mjs` fetch fd protocol — implement request/response serialization (4B len + JSON) in mjs binary; `fetch()` returns Promise that blocks on fd read; handle timeout/error JSON gracefully|V50
T134|x|agent-side fetch proxy — in `shell_exec` fork path: create `socketpair(AF_UNIX, SOCK_STREAM)` before fork; pass child end as `MJS_FETCH_FD`; parent monitors fd via select alongside child stdout pipe; on request: parse JSON, check allowed_hosts (V46), curl, write response; close on child exit|V49,V50,V46
T135|x|test: mjs fetch proxy end-to-end — agent spawns `shell_exec("mjs -e 'await fetch(...)'")`; mock HTTP server; verify request goes through proxy, response arrives in mjs stdout; verify denied host returns 403|V49,V50,V47
T136|x|test: mjs pipeline composability — `shell_exec("mjs -e '...' \| grep ...")` works; fetch fd ⊥ interfere w/ stdout piping|V51
T137|x|Makefile: `mjs` binary target — compile mquickjs evaluator, install to `build/mjs`; `make install` copies to `/usr/local/lib/cclaw/mjs`|V48
T138|x|template embedding — `templates/` dir w/ `.md`, `.txt`, `.sql` files; build-time script converts to C byte arrays in `build/templates.h`; replace inline `static const char*` strings in config.c, context.c, db.c w/ `#include "templates.h"` refs; Makefile rule: `build/templates.h` depends on `templates/*`|§C
T139|x|Telegram admin auth — `admin_chat_ids[]` in config; `telegram_is_admin(chat_id)` check; non-admin messages route to agent normally; admin commands intercepted before inbox_insert|V53,§I
T140|x|Telegram admin command parser — `/config`, `/key`, `/whitelist` prefix detection in poller thread; dispatch to admin handlers; non-command messages pass through to agent|V53
T141|x|`/key` dialog — inline keyboard: select provider (OpenRouter, Gemini, custom) → ForceReply prompt for key → write to env file (`/etc/cclaw/env` or config-specified path) → confirm; key value ⊥ logged, ⊥ stored in DB, ⊥ enters inbox|V52
T142|x|`/config model` dialog — inline keyboard: list configured providers → select → ForceReply for model name → update `config.json` provider entry → confirm + reload config in daemon|§I
T143|x|`/config endpoint` dialog — ForceReply for base_url + provider name → validate URL format → write to config.json → reload|§I
T144|x|`/whitelist` dialog — inline keyboard: list agents → select → show current `allowed_hosts[]` → ForceReply for new host → append to agent config → reload; also support `/whitelist remove`|V46,§I
T145|x|test: admin commands — verify key write bypasses DB entirely; verify non-admin chat_id rejected; verify config reload picks up changes|V52,V53
T146|x|`approvals` table — schema: id, session_id, agent_name, type (whitelist_host\|create_agent\|model_change\|tool_enable), payload TEXT (JSON), status (pending\|approved\|denied), admin_chat_id, created_at, resolved_at|V54,§D
T147|x|`approval_request` tool — agent calls w/ type + payload (e.g. `{"type":"whitelist_host","host":"api.example.com"}`); writes to `approvals` table; daemon delivers inline keyboard to admin via Telegram; agent receives tool_result "pending approval — waiting for admin"|V54
T148|x|approval delivery — daemon detects new pending approval → sends formatted message + Approve/Deny buttons to all `admin_chat_ids[]`; callback_data encodes approval id|V54,V53
T149|x|approval callback handler — admin taps Approve → daemon applies change (write config, reload), updates approval row, posts result to agent inbox; Deny → posts denial to inbox; agent session transitions idle→running on inbox signal|V54,V25
T150|x|agent-initiated agent creation — agent proposes new agent via `approval_request` type `create_agent` w/ payload (name, model, system_prompt, tools, allowed_hosts); admin approves → daemon writes `agents/<name>/agent.json` + `system.md` + seeds DB row|V54,V20
T151|x|test: approval flow end-to-end — agent requests whitelist host → approval pending → mock admin approve → config updated → agent inbox receives confirmation; also test deny path + unauthorized approval attempt|V54,V53

T152|x|memory blocks table — `memory_blocks(id, agent_name, label, value TEXT, description TEXT, char_limit INT DEFAULT 5000, read_only INT DEFAULT 0, created_at, updated_at)`; default: zero blocks — agent creates via `memory_create`; optionally pre-seeded from `agent.json` `memory_blocks[]`; DB authoritative after seed|§D,V55
T153|x|memory tools — `memory_create(label, description, value?)`, `memory_append(label, content)`, `memory_replace(label, old, new)`; operate on block `value` only; respect `read_only` flag; persist to DB immediately|§I,T152
T154| |system prompt memory injection — at context build, render blocks into prompt as labeled sections w/ metadata (label, description, chars_used/limit); agent sees structure, knows what each block is for|T152,T122
T155| |drop flat `soul`/`memory` columns from agents table — migrate existing content to `persona`/`human` blocks on first access|T152

Test tiers (Makefile targets):
- `make test` — unit tests (no network, no LLM, fast, always run)
- `make test-integration` — mock-server tests (civetweb in-process, no external deps, ~seconds)
- `make test-e2e` — live LLM tests (require API key, hit real endpoints, skip if key missing)

## §B BUGS
id|date|cause|fix

## §F FUTURE
- Web chat: civetweb serves chat UI (SSE streaming for partial responses, session select/create, message history); block streaming to browser as assistant generates; replaces status-only page
- Intra-turn steering: agent checks inbox between tool executions, injects new messages into context mid-loop (Pi model: steering = interrupt, follow-up = queue until stop); complicates V18 atomic consumption — design carefully
- Session curation: mid-session compaction (summarize arbitrary entry ranges, re-parent tail), prune failed tool-call loops, automated "curation agent" that cleans up long-running sessions
- Session recreation: compose new sessions from cherry-picked existing entries (join table `curated_branches(session_id, entry_id, position)`) — avoids copying, entries stay immutable, new summary entries fill gaps
- Curation agent: background sub-agent that operates on another session's branch — identifies noise (repeated failures, dead-end tool calls), summarizes or removes them, produces a cleaner branch for continued work
- Telegram/non-CLI sessions can't easily branch interactively — curation agent fills that gap
- Seccomp-bpf: defense-in-depth syscall filtering for agent processes (block fork/execve/ptrace/mount — force all execution through shell_exec tool); both platforms have `CONFIG_SECCOMP_FILTER=y`; needs per-arch filter defs (ARM64 vs ARMv5) or `libseccomp`; add once daemon model stable and syscall needs empirically known
- Filtered shell network: `CLONE_NEWNET` stays on; network from shell only via `mjs` fetch proxy (V49-V50); if raw socket needed (e.g. `git clone`), future option: userns iptables w/ IP whitelist — but mjs fetch covers most cases
- Multi-arch releases: GitHub Actions matrix with Docker containers (arm64, armhf, amd64) producing tarballs; binary + "install libcurl" README; not Zig-style cross-compile — native build per target with system libcurl; ARMv5TE (Pogoplug) likely needs dedicated Debian armel container
- Cost tracking: save OpenRouter `cost` field when present; for direct providers, compute from per-model rate table (input/output/cache_read/cache_write $/M tokens); store per-entry in metadata; aggregate per-session and per-agent; expose via web dashboard and `db_query`
- Extension system via MicroQuickJS: extensions are JS modules loaded at session/agent start; can register tools, hook events (before/after tool call, before LLM request), modify system prompt; loaded from `agents/<name>/extensions/*.js` or global `extensions/`; extensions have access to `http_fetch` (with policy) and filesystem (workspace-scoped); replaces need for native C plugin system
- MCP extension: reference JS extension that speaks Model Context Protocol over stdio/HTTP; discovers remote tools, registers them via the extension API; ships as a built-in example extension
- OpenAI Device Code auth: OAuth 2.0 Device Authorization Grant (RFC 8628) for ChatGPT/Codex subscription access without API key; show URL + code → user approves on phone/laptop → poll for token; works headless (no browser callback); store access+refresh tokens in DB; auto-refresh on expiry; identify as `cclaw/<version>` User-Agent
- System prompt in DB: move system prompts from `agents/<name>/system.md` to a `prompts` table (id, name, template TEXT, created_at); template vars `{session_id}`, `{date}`, `{agent_name}`; agents reference prompt by name/id in config; allows runtime editing without filesystem access; keep file-based loading as fallback/import path
- Workspace model refinement: each agent owns `./workspace/<agent_name>/`; agents can share workspace via config (`"workspace": "other_agent_name"`); no global "default" workspace; `/tmp/cclaw-<session_id>/` always writable (tool overflow, scratch); workspace + tmp dir are the only writable paths under landlock/namespace
- Auto-recall (FTS5): at context build, extract keywords from current user message → FTS5 search over entries + memory_blocks → inject top-N relevant hits as `<recalled_context>` section in system prompt; zero agent effort, system-level; configurable threshold + max tokens budget
- Vector recall (NEXT): embedding-based semantic search over entries + memory_blocks + workspace files; hybrid w/ FTS5 (RRF merge); requires embedding model (local or API); `passages` table (text, embedding BLOB, source_type, source_id); agent tool `memory_search(query)` for explicit recall; auto-recall injects top hits same as FTS5 path
