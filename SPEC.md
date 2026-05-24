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
- file: `config.json` — provider, model, tokens, channels, workspace, db_path
- file: `agents/<name>/agent.json` — per-agent: model, workspace, tools, max_iterations, allowed_hosts
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
- js binding: `http_fetch(url, {method, headers, body})` — C-provided, enforces agent `allowed_hosts` + SSRF protection; sole network path from JS runtime
- tool: `spawn_agent` — fork sub-agent process (accepts `background` param, default blocking)
- tool: `db_query` — execute read-only SQL against cclaw.db (SELECT only, no mutations)
- tool: `web_fetch` — HTTP GET URL, extract text from HTML, external input protection wrapper
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
V22: ∀ agent process → landlock applied at fork: write restricted to agent workspace; read-only `/usr`, `/etc`, curl CA bundle; graceful fallback if landlock unavailable (Pogoplug kernel 6.19.9 has `CONFIG_SECURITY_LANDLOCK=n` — log warning, continue without)
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
V40: ∀ context_build → tool_result content truncated to 50KB / 2000 lines (whichever first) when building LLM messages; full result preserved in DB entry (searchable via FTS5); truncated results get suffix `[truncated — {N} bytes / {M} lines omitted, use search to find full output]`
V41: ∀ LLM request → built via `CURLOPT_READFUNCTION` streaming from SQLite cursor; ⊥ load full session into memory; two-pass: (1) plan entry IDs + cut point, (2) stream JSON from cursor; per-agent memory footprint ≤ arena + curl buffers (~2-5MB), not session-proportional
V42: ∀ heartbeat → daemon triggers agent run w/ heartbeat prompt; agent reads `HEARTBEAT.md` if present, acts on tasks; response `HEARTBEAT_OK` = sentinel (suppressed, ⊥ delivered to channel); any other response → deliver to channel via `last_route`
V43: ∀ tool dispatch → track last N calls (name + args hash + result hash); if same call repeated ≥ 5× w/ no progress → inject warning into tool_result; ≥ 10× → force-stop agent loop w/ error ("tool loop detected")
V44: ∀ Telegram group msg → if agent response contains `[NO_REPLY]` → suppress delivery (⊥ send to chat); agent decides relevance per system prompt guidance
V45: ∀ agent response → if `stop_reason == stop` & no tool_calls & response is plan-only (bullet list + "I'll do X" promise, no tool action taken) → re-prompt once: "Do not restate the plan. Act now: take the first concrete tool action. If blocked, state the blocker in one sentence."

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
T105|.|tool result truncation in `context_build` — shared `truncate_result(buf, len)` util enforcing 50KB/2000 lines on tool_result messages before sending to LLM; full results stay in DB|V40
T106|.|streaming request planner — `context_plan()` returns ordered entry ID list + cut point + token budget from SQLite (pass 1, no content loaded)|V41,V7,V8
T107|.|`RequestStreamer` state machine — phases: preamble → entries (cursor step + reshape per-entry JSON) → tools → close; implements `CURLOPT_READFUNCTION` callback|V41
T108|.|integrate streaming request into `agent.c` — replace `llm_build_request` (full-buffer) with `RequestStreamer`; verify retry resets cursor|V41,V2
T109|.|heartbeat agent trigger — daemon injects heartbeat user msg into session inbox, forks agent; heartbeat prompt: "Read HEARTBEAT.md if present. Follow it. If nothing needs attention, reply HEARTBEAT_OK."|V42,T34
T110|.|`HEARTBEAT_OK` sentinel suppression — daemon checks final assistant response; if content == `HEARTBEAT_OK` → suppress delivery (⊥ send to channel); else deliver normally|V42,V26
T111|.|`HEARTBEAT.md` workspace file — optional; defines proactive tasks (reminders, checks, maintenance); agent reads via `file_read` during heartbeat turn|V42
T112|.|tool loop detection — hash(name+args) history ring buffer (last 30 calls per session); on dispatch, check streak; ≥ 5 same → inject warning in result; ≥ 10 → return error + break loop|V43
T113|.|`[NO_REPLY]` suppression — Telegram group delivery checks response for marker; if present, skip `sendMessage`; system prompt instructs agent when to use it|V44
T114|.|planning-only retry — after final assistant response w/ no tool_calls, detect plan-only pattern (bullets + promise verbs, no action); re-prompt once w/ act-now instruction; max 1 retry|V45
T115|.|CLI mid-turn progress — always-on: stream intermediate assistant text + tool call names/args as they execute; tool results truncated aggressively for display (shorter than V40 LLM limit); `--debug` adds raw JSON req/resp on top|§I.cmd

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
- Filtered shell network: if `shell_exec` ever needs network (e.g. `git clone`), use `CLONE_NEWNET` + userns iptables with IP whitelist (resolved from `allowed_hosts`); currently unnecessary — JS `http_fetch` binding covers network needs without shell access
