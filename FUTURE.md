# FUTURE — Deferred Ideas

Parking lot for features explicitly out-of-scope in current SPEC.md.
Move to §T when ready to implement.

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

## LLM-based Compaction
- When context window fills, summarize old messages via LLM call
- Pi pattern: `shouldCompact()` when `contextTokens > contextWindow - reserveTokens`
- Keep ~20k recent tokens, summarize the rest
- Iterative: update previous summary rather than re-summarize everything
- Cut at valid turn boundaries (never mid-tool-call)
- Store as `compaction` entry in session tree

## Cross-compile for Pogoplug
- Target: Pogoplug V4 (ARMv5TE, 128MB RAM, Debian Bookworm armel)
- musl static link or armel dynamic
- Reduced feature set (no mquickjs? smaller arena?)

## Multi-model Routing
- Different models for different tasks (cheap for compaction, expensive for reasoning)
- Per-session model override stored in DB

## FTS5 Search Tool
- Expose message search as agent tool (`search_history`)
- Agent can search its own past conversations

## Landlock Shell Restriction
- Use Linux Landlock LSM to restrict `shell_exec` filesystem access
- Limit to workspace dir + explicit allowlist (e.g., `/usr/bin`, `/tmp`)
- Prevents agent from escaping workspace via shell even without path checks
- Reference: OpenClaw's crabbox sandbox model

## JS Extension System

### Tool Authoring via mquickjs

The agent can define new tools at runtime via `js_define_tool`. These persist per-session (replayed on reload via T33). Questions:

- Should JS tools be able to call other tools? (tool composition)
- If yes, inject a `callTool(name, args)` global that dispatches through the tool registry
- A JS-defined tool that calls `fetch`, transforms, then calls `file_write` = mini-agent in JS
- Schema: agent provides JSON schema when defining the tool, or infer from function signature?

### Workspace Scripts as Tools

Drop a script in `workspace/tools/`, auto-discover or explicit register. Any language. Input via stdin/env, output via stdout. Zero ceremony.

- Discovery: scan `tools/` dir on session start, register each executable as a tool
- Convention: `tools/my_tool.sh --schema` prints JSON schema, `tools/my_tool.sh` runs with args on stdin
- Or: `tools/my_tool.json` sidecar with name/description/schema, points to executable

### Both Coexist

| Mechanism | Good for | Limitations |
|-----------|----------|-------------|
| `js_define_tool` | Data transformation, logic, composing other tools | No system access (unless we inject it) |
| Workspace scripts | System access, external binaries, pip/npm packages | Subprocess overhead, no in-process state |

## JS Runtime Capabilities

### fetch()

Synchronous (mquickjs is blocking). Wraps libcurl. Respects V2 (retry/backoff).

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
- Pogoplug target (128MB RAM): keep it tight, maybe 2MB max

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
