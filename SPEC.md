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
- decentralized execution with CAS session locking (no central runner thread)

## §I INTERFACES
- cmd: `./build/cclaw --cli` → stdin/stdout REPL, creates/resumes session in shared DB
- cmd: `./build/cclaw --cli --debug` → raw LLM req/resp JSON to stderr
- cmd: `./build/cclaw` → daemon mode (Telegram poller + civetweb status page + janitor sweep)
- cmd: `./build/cclaw config.json` → explicit config file
- cmd: `./build/cclaw --sub-agent --session-id=X --task="..."` → sub-agent process
- env: `OPENROUTER_API_KEY` required (minimum to run)
- env: `CCLAW_PROVIDER`, `CCLAW_MODEL`, `CCLAW_TELEGRAM_TOKEN`, `CCLAW_DB_PATH`, `CCLAW_WEB_PORT`
- file: `config.json` — provider, model, tokens, channels, workspace, db_path, stale_lock_timeout
- file: `agents/<name>/agent.json` — per-agent: model, workspace, tools, max_iterations
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
- tool: `spawn_agent` — fork sub-agent process (accepts `background` param, default blocking)
- tool: `db_query` — execute read-only SQL against cclaw.db (SELECT only, no mutations)
- tool: `web_fetch` — HTTP GET URL, extract text from HTML, external input protection wrapper
- db: `cclaw.db` — SQLite 3.53, WAL, FTS5, JSON functions

## §D DATA
entries table: `id`, `session_id`, `parent_id`, `turn_id`, `created_at`, `data TEXT NOT NULL` (JSON), generated columns: `type`, `role`

Entry `data` format (discriminated by `$.type`):
- `{"type":"message","role":"user","content":"..."}`
- `{"type":"message","role":"assistant","content":[{"type":"text","text":"..."},{"type":"tool_call","id":"...","name":"...","arguments":{...}}],"model":"...","usage":{"input":N,"output":N},"stop_reason":"stop|tool_use|length|error"}`
- `{"type":"message","role":"tool_result","tool_call_id":"...","name":"...","content":"...","is_error":false}`
- `{"type":"message","role":"system","content":"..."}`
- `{"type":"compaction","summary":"...","first_kept_id":N}`

inbox table: `id`, `session_id`, `source TEXT`, `payload TEXT`, `created_at`, `consumed INTEGER DEFAULT 0`

sessions table: `id`, `name`, `leaf_id`, `agent_name TEXT`, `state`, `lock_holder`, `lock_acquired_at`, `error_count`, `created_at`, `updated_at`

Agent config on disk (`agents/<name>/`):
- `agent.json` — model override, tool whitelist, max_iterations, workspace path
- `system.md` — system prompt template (supports `{session_id}`, `{date}`, `{agent_name}`)
- `skills/` — per-agent skill files (markdown, injected into system prompt)
- `notes/` — persistent knowledge files agent can `file_read`

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
V11: ∀ Telegram msg → chunk at 4096 chars, split at paragraph then sentence boundaries
V12: workspace ! per-agent (config `workspace` field per agent, fallback to `./workspace/{agent_name}`)
V13: sub-agent spawn → blocking (default): parent waits, result returned as tool output; background: parent continues, sub-agent posts name+id to parent inbox on completion (parent queries details via `db_query`)
V14: session tree structure → entries w/ `id` + `parent_id` (Pi model); branching structure in DB even if branching UI deferred
V15: ∀ tool result → optionally wrap in `<tool_result name="X">...</tool_result>` tags to sanitize external data (configurable per tool)
V16: ∀ agent_run → session must be in 'running' state, acquired via atomic CAS lock matching thread/process ID
V17: ∀ turn → entries share a `turn_id`; incomplete turns intercepted, structure synthesized, and system notice injected via `context_build`
V18: ∀ inbox message → consumed exactly once into session entries via single atomic SQLite transaction (`BEGIN EXCLUSIVE`)
V19: ∀ session state transition → executed via strict atomic UPDATE with WHERE clauses targeting expected states to prevent TOCTOU
V20: ∀ session → `agent_name` identifies agent; config loaded from `agents/<name>/` on disk; fallback to global config when NULL

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
T23|x|Telegram poller (`telegram.c`) — getUpdates loop in thread|§I.api
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
T37|x|`spawn_agent` tool — fork+exec sub-agent process|V3,V13
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
T54|.|integration test: provider fallback (kill primary, verify fallback fires)|T45
T55|x|DB Schema: sessions CAS columns (state, lock_holder, lock_acquired_at, error_count) + inbox table|V16,V18
T56|.|CAS Acquire/Release (`session_try_acquire`)|V16,V19
T57|.|agent.c Turn Tagging (assign `turn_id` via index query)|V17
T58|.|context.c Incomplete Turn Interception (synthetic failure + notice)|V17
T59|.|Janitor Sweep Logic (stale locks, orphan pending recovery)|V16,V19
T60|.|Anti-Crash Loop Limit (`error_count` tracking, quarantine ≥ 3)|V19
T61|.|Phase A Integration (verification matrices for overlap rejection)|V16
T62|.|inbox Core Primitives (`inbox_insert`, `inbox_peek`)|V18
T63|.|Atomic Move Transaction (`inbox_consume_into_entries`)|V18
T64|.|Background sub-agent completion (post result to parent inbox)|V13,V18
T65|.|`spawn_agent` blocking mode (wait on child, return result as tool output)|V13,V3
T66|.|Verification Tests (atomic rollbacks mid-consumption)|V18
T67|.|CLI workspace triggers (acquire/release keep-alive framework)|V16
T68|.|Telegram intake handlers (inbox_insert + trigger local lock)|V16
T69|.|Cron process actions (transactional inbox wrappers)|V16
T70|.|Integration Test: parallel high-throughput network payloads|V16,V18
T71|.|Web console updates (state metrics, lock holders, backlog depths)|§I.web
T72|.|CLI terminal resume paths (echo unread inbox counts)|§I.cmd
T74|.|Bind runtime parameters (`stale_lock_timeout`) to config|§I.file
T75|.|agent discovery — scan `agents/` dir, list available agents by name|V20
T76|.|agent config loader — read `agents/<name>/agent.json`, merge w/ global config|V20,V12
T77|.|system prompt loader — read `agents/<name>/system.md`, template vars `{session_id}`, `{date}`, `{agent_name}`|V20,T46
T78|.|per-agent tool whitelist — filter tool registry by agent config|V20,§I.tool
T79|.|session↔agent binding — session_create accepts agent_name, load config from disk at agent_run|V20
T80|.|skill loader — scan `agents/<name>/skills/*.md`, inject into system prompt|V20

## §B BUGS
id|date|cause|fix

## §F FUTURE
- Session curation: mid-session compaction (summarize arbitrary entry ranges, re-parent tail), prune failed tool-call loops, automated "curation agent" that cleans up long-running sessions
- Session recreation: compose new sessions from cherry-picked existing entries (join table `curated_branches(session_id, entry_id, position)`) — avoids copying, entries stay immutable, new summary entries fill gaps
- Curation agent: background sub-agent that operates on another session's branch — identifies noise (repeated failures, dead-end tool calls), summarizes or removes them, produces a cleaner branch for continued work
- Telegram/non-CLI sessions can't easily branch interactively — curation agent fills that gap
