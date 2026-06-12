# FUTURE — Deferred Ideas

Parking lot for features explicitly out-of-scope in current SPEC.md.
Move to §T when ready to implement.

## Auto-Recall: Diversity + Per-Hit Metadata
- Cap hits per source session (1–2) so one session can't fill all recall slots
  (GROUP BY session_id, keep best-ranked per session)
- Tag each hit with session date + first-message snippet so the agent can judge
  whether to expand: `[session 6, 2026-06-10, "good evening assistant."] ...`
- Pairs with the existing "query entries by session_id" hint in the recall header

## Auto-Recall: Recency Tiebreak
- BM25 has no concept of time; an old session outranks yesterday's at equal score
- Blend rank with age: `ORDER BY rank + (age_days * 0.05)` or similar small
  additive penalty — bias toward recent sessions when relevance is close
- Keep pure BM25 for large score gaps (genuinely better match wins regardless)

## WhatsApp Business API Channel
- Webhook endpoint via civetweb: `POST /webhook/whatsapp`
- Verify endpoint: `GET /webhook/whatsapp?hub.verify_token=...`
- Send via Graph API (`POST https://graph.facebook.com/v21.0/{phone_id}/messages`)
- phone_number → session_id mapping

## Web Dashboard (Full)
- Session list, message viewer, sub-agent status, cron jobs
- Web-based chat interface
- Simple HTML — no JS framework, server-rendered or minimal vanilla JS

## Branching UI
- Visual tree in web interface w/ user-selectable branching
- Optional summarization on branch navigate (Pi model: summarize abandoned branch)
- Pi pattern: `collectEntriesForBranchSummary` → find common ancestor → summarize divergent path
- Branch summary stored as entry type in session tree

## Session Curation Agent
- Autonomous agent that periodically performs branching/compaction for other agents
- Summarizes old branches, prunes dead ends
- Runs as sub-agent on schedule (cron)

## Cross-compile for Legacy Embedded Targets
- Target: ARMv5TE and similar (128MB RAM)
- musl static link or armel dynamic
- Reduced feature set (no mquickjs? smaller arena?)

## Multi-model Routing
- Different models for different tasks (cheap for compaction, expensive for reasoning)
- Per-session model override stored in DB

## Search & Long-term Memory

### search_history tool
- Dedicated tool wrapping FTS5 search over agent's own sessions
- Higher-level than raw `db_query` — returns formatted results with context
- Search across all sessions, not just current

### Cross-agent search
- Agent can search other agents' sessions (with permission)
- Use case: coordinator agent reviewing sub-agent work
- Requires read access to other agent DBs (daemon-mediated or namespace bind-mount)

### Long-term memory ideas
- Semantic search over past conversations (embedding-based, not just keyword FTS5)
- Auto-summarization of old sessions into memory blocks
- "What do I know about X?" tool that searches memory + history
- Memory consolidation agent (background, merges/prunes memory blocks over time)
- Episodic memory: key events/decisions tagged and retrievable by topic

### Shared memory blocks
- Problem: memory_blocks are scoped by agent_name in cclaw.db — no cross-agent visibility
- Approach: `shared_memory_blocks` table in cclaw.db (daemon-owned)
- Read: agents get read-only access via `shared_memory_read` tool (or bind-mount cclaw.db ro)
- Write: mediated by daemon — agent requests write via exit code 4 or a dedicated tool that posts to inbox
- Use cases: shared knowledge base, project context, team conventions, coordination state
- Scoping: optional namespace/tag per block so agents can subscribe to relevant shared memory only

## JS Extension System

MicroQuickJS is the extension language for CClaw. Current foundation: `js_define_tool` (runtime tool creation) and `js_eval` (one-shot execution). The full extension system will cover:

### Planned Extension Points

| Extension type | What it does | Example |
|---------------|--------------|---------|
| Tools | Register new tools callable by the agent | Custom API wrappers, data transforms |
| Channel extensions | New input/output channels beyond Telegram/CLI | Discord, Slack, email, webhooks |
| Agent loop hooks | Before/after LLM call, before/after tool dispatch | Logging, cost tracking, guardrails |
| Skills | Prompt fragments loaded into system prompt | Domain knowledge, persona, instructions |
| Prompt management | Dynamic system prompt composition | Context-aware prompt assembly |

### Extension Loading

Extensions are JS modules loaded at agent startup from `agents/<name>/workspace/extensions/`. Each declares what it hooks into. Loaded fresh each turn (no persistent state beyond what's in DB).

### Reference: Pi Extension Model

See `reference/pi-extensions.md` for Pi's approach:
- Extensions declare lifecycle hooks (onMessage, onToolCall, beforeRequest, afterResponse)
- Extensions can modify the message array before LLM call
- Extensions can register tools dynamically
- Extensions have scoped permissions

CClaw adaptation: same concepts, implemented in MicroQuickJS instead of TypeScript, with SQLite for state instead of in-memory.

### Tool Authoring via mquickjs (foundation ✓)

`js_define_tool` lets agents create tools at runtime. Persists per-session. Remaining work:
- Tool composition: JS tools calling other tools via `callTool(name, args)`
- Schema inference vs explicit declaration
- Workspace script auto-discovery (`workspace/tools/*.sh`)

### fetch() (foundation ✓)

Synchronous fetch via UDS proxy. Respects allowed_hosts. Remaining work:
- Response size caps (heap bounded)
- Configurable timeout per request
- Streaming for large responses

## JS Runtime Capabilities

### fetch() ✓ (implemented)

Synchronous (mquickjs is blocking). Wraps libcurl via UDS proxy. Respects allowed_hosts.

- URL allowlist? Or inherit agent's permissions?
- Response size cap (heap is bounded)
- Timeout per request

### fs (readFile/writeFile)

Same workspace restriction as `file_read`/`file_write` tools (V1). Injected as globals.

- `fs.readFile(path)` → string
- `fs.writeFile(path, content)` → boolean
- Path resolution relative to agent workspace

### Heap Size

1MB is fine for pure computation. With fetch/fs:

- HTTP response + parsed JSON + processing = easily 2-4MB
- Make heap configurable per-agent in config.json
- Default 1MB for `js_eval`, 4-8MB for tools with I/O
- Legacy ARM targets (128MB RAM): keep it tight, maybe 2MB max

## Self-Reflection / Introspection

### DB Access from JS

The agent's SQLite DB contains sessions, messages, tool results, sub-agent state. A JS tool with DB read access enables:

- **Self-reflection**: query own conversation history, count tokens used, review past decisions
- **Cross-session search**: FTS5 search over all sessions ("when did I last discuss X?")
- **Sub-agent coordination**: check status of spawned agents without `check_agent` tool
- **Analytics**: token usage over time, tool call frequency, error rates

Implementation: inject `db.query(sql)` global that runs read-only queries against cclaw.db.

- Read-only (no INSERT/UPDATE/DELETE from JS)
- Or: separate `db.read(sql)` and `db.write(sql)` with write restricted to agent's own tables
- Result as array of objects (JSON-friendly)

### Agent State Globals

Inject read-only context about the current agent:

```javascript
agent.id          // current agent ID
agent.session_id  // current session
agent.workspace   // workspace path
agent.model       // model name
agent.parent_id   // parent agent (if sub-agent)
agent.depth       // sub-agent depth (0 = root)
```

Enables JS tools that behave differently based on context (e.g., a tool that's more conservative at depth > 0).

## Extension System (OpenClaw/Pi Patterns)

### OpenClaw Model

- Extensions are TypeScript modules with a standard interface
- Each extension declares: name, description, schema, handler
- Extensions can subscribe to events (message received, tool called, etc.)
- Extensions have scoped permissions (which APIs they can access)

### Pi Model

- Session tree with branching
- Tools defined in TypeScript, hot-reloaded
- Sub-agents as first-class concept
- Clean separation: agent loop / tool dispatch / session management

### CClaw Adaptation

- C is the core (agent loop, DB, HTTP, tool dispatch)
- mquickjs is the extension language (replaces TypeScript role)
- Shell scripts are the escape hatch (replaces "any language" flexibility)
- SQLite is the coordination layer (replaces message queues / IPC)
- No hot-reload needed — JS tools are eval'd fresh each call (or replayed from DB)

## HTTP Transport Abstraction

Extract curl out of the agent process behind a swappable `HttpTransport` interface:

```c
typedef struct {
    int (*request)(const char *url, const char *method,
                   const char *headers, const char *body,
                   char **response, void *ctx);
    void *ctx;
} HttpTransport;
```

Implementations:
- `http_transport_curl()` — direct libcurl (current, for standalone/testing/beefy hardware)
- `http_transport_uds(fd)` — parent holds warm TLS connections, child talks plaintext over UDS
- `http_transport_wasm()` — calls Workers `fetch()` host import for Cloudflare WASM target

Benefits:
- Pogoplug: eliminates per-turn TLS handshake (~800ms on ARMv5TE) — parent keeps connection warm
- Cloudflare Workers: agent compiles to WASM without libcurl dependency
- Same agent binary, different transport selected at startup via `CCLAW_HTTP_TRANSPORT` env

Pattern mirrors existing shell networking proxy (shell→agent UDS) but one level up (agent→parent UDS).

## Alternative Proxy Mechanisms for Static/Go/Rust Binaries

Current proxy relies on `LD_PRELOAD=libcclaw_net.so` to intercept libc
`connect()`/`getaddrinfo()`. This works for dynamically-linked programs
(curl, git, python, node) but not for statically-linked or Go/Rust binaries
that make raw syscalls. Those get zero network (`CLONE_NEWNET` hard backstop).

### seccomp-unotify (kernel ≥5.9)
Intercept the `connect()` syscall in the parent process via seccomp user
notification (`SECCOMP_RET_USER_NOTIF`). The parent reads the target address
from the child's memory, performs the proxy logic, and injects the connected
fd back. Works for everything regardless of linking — no .so needed. Requires
kernel ≥5.9 and is significantly more complex (~500 LOC).

### Application-level proxy env vars
Set `HTTP_PROXY`/`HTTPS_PROXY`/`ALL_PROXY` pointing to a proxy endpoint.
Problem: inside `CLONE_NEWNET` there's no TCP loopback, so the proxy would
need to listen on a UDS and programs would need native UDS proxy support
(most don't). Go's net/http respects `HTTP_PROXY` but not over a Unix socket
natively.

### Helper-script wrapper
Provide a `cclaw-fetch` binary in `/bin/` inside the namespace that speaks
the proxy UDS protocol directly. The LLM is instructed to use `cclaw-fetch`
instead of `curl`/`wget`. Anything not using the wrapper gets zero network
(kernel-enforced). Simple but requires LLM cooperation and doesn't help tools
like `git clone` that internally resolve + connect.

## Session Sweeper

Writes summary entries as new nodes in the parent/leaf tree (git-squash style),
never overwrites existing entries. Summaries reference the head entry-id of the
branch they summarize. Dead ends preserved as structured negative results,
invisible to context assembly, fully indexed for recall. Runs on heartbeat tick
outside the recency window.

## sqlite-vec + Binary Quantization

After FTS5 is shipping. 384-dim vectors, brute-force Hamming over
binary-quantized index, full-precision rerank of top-N, RRF fusion with BM25 and
recency term. Embedding at write time via cheap API (async, `needs_embedding`
flag, broker sweep). FTS5-only remains the offline fallback. Guide users through
local-model setup when they want it.