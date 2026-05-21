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

## §I INTERFACES
- cmd: `./build/cclaw --cli` → stdin/stdout REPL, creates/resumes session in shared DB
- cmd: `./build/cclaw --cli --debug` → raw LLM req/resp JSON to stderr
- cmd: `./build/cclaw` → daemon mode (Telegram poller + civetweb status page)
- cmd: `./build/cclaw config.json` → explicit config file
- cmd: `./build/cclaw --sub-agent --session-id=X --task="..."` → sub-agent process
- env: `OPENROUTER_API_KEY` required (minimum to run)
- env: `CCLAW_PROVIDER`, `CCLAW_MODEL`, `CCLAW_TELEGRAM_TOKEN`, `CCLAW_DB_PATH`, `CCLAW_WEB_PORT`
- file: `config.json` — provider, model, tokens, channels, workspace, db_path
- api: Telegram Bot API (long-poll `getUpdates`, `sendMessage`, `sendChatAction`)
- api: OpenAI-compatible `POST /chat/completions` (any provider)
- web: `GET /` → minimal status page (active sessions, uptime)
- tool: `shell_exec` — run cmd, return stdout/stderr
- tool: `file_read` — read file (workspace-restricted)
- tool: `file_write` — write file (workspace-restricted)
- tool: `js_eval` — execute JS in sandboxed mquickjs
- tool: `js_define_tool` — register JS fn as callable tool (session-persistent)
- tool: `spawn_agent` — fork sub-agent process
- tool: `check_agent` — read sub-agent result from DB
- db: `cclaw.db` — SQLite 3.53, WAL, FTS5, JSON functions

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
V12: workspace ! per-agent (config `workspace` field per agent, fallback to `./workspace/{agent_id}`)
V13: sub-agent result delivery → only on explicit `check_agent` call (never auto-inject)
V14: session tree structure → entries w/ `id` + `parent_id` (Pi model); branching structure in DB even if branching UI deferred
V15: ∀ tool result → optionally wrap in `<tool_result name="X">...</tool_result>` tags to sanitize external data (configurable per tool)

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
T18|.|`file_read` tool — workspace path restriction|V1
T19|.|`file_write` tool — workspace path restriction|V1
T20|.|CLI REPL (`cli.c`) — read line, send to agent, print response|§I.cmd
T21|.|CLI debug mode — raw req/resp JSON to stderr|§I.cmd
T22|.|CLI session selection (create new / resume existing)|§I.cmd
T23|.|Telegram poller (`telegram.c`) — getUpdates loop in thread|§I.api
T24|.|Telegram send + typing indicator (every 4s while working)|V11
T25|.|Telegram offset persistence in DB (survives restart)|§I.api
T26|.|Telegram chat_id → session routing|§I.api
T27|.|Telegram exponential backoff on transient errors|V2
T28|.|civetweb integration — start server, register routes|§I.web
T29|.|status page — active sessions, uptime, sub-agent status|§I.web
T30|.|vendor mquickjs, integrate into build|§C
T31|.|`js_eval` tool — execute code, return result|V5
T32|.|`js_define_tool` — register JS fn as tool (session-persistent)|§I.tool
T33|.|JS context replay on session reload|T32
T34|.|heartbeat timer thread + system msg injection|§C
T35|.|cron table + scheduler thread|§I.db
T36|.|`cron_set`/`cron_list`/`cron_remove` tools|T35
T37|.|`spawn_agent` tool — fork+exec sub-agent process|V3,V13
T38|.|`check_agent` tool — read result from DB|V13
T39|.|sub-agent lifecycle (limits, cleanup, crash isolation)|V3
T40|.|token estimation (chars/4 heuristic)|V7
T41|.|graceful shutdown (SIGINT/SIGTERM)|§C
T42|.|systemd service file|§C
T43|.|error handling — 429 retry, context overflow detect, JSON parse failure recovery|V2,V10

## §B BUGS
id|date|cause|fix
