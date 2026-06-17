# Extension System

## Overview

An extension is a single JS package that can provide any combination of:

| Component | Runs in | Lifecycle |
|-----------|---------|-----------|
| Tools | agent process (inside `agent_run`) | per-turn, fresh each turn |
| Hooks | agent process (inside `agent_run`) | per-turn, fresh each turn |
| Channel | separate process (daemon-managed) | long-lived, restarted on crash |

Tools and hooks are **agent-side** — they execute inside the agent process as part of `agent_run`, same as built-in tools. They share the agent's QuickJS heap, permissions, and turn lifecycle.

Channel components are **special** — they run as separate processes with their own event loop, communicating with the daemon via the channel API (V98–V108). A channel extension provides a binary (or script) that the daemon launches and monitors.

An extension can provide all three, or just one. Installing an extension makes all its components available.

## JS Runtime: Dialect & Globals

All JavaScript in CClaw — extension tools/hooks, `js_define_tool` handlers, and the `js_eval` tool — runs in the same MicroQuickJS engine. It is **ES5**, not modern JS:

- Use `var` — `const` and `let` are **not** parsed (they raise `SyntaxError: unexpected character in expression`).
- No arrow functions (`=>`), template literals (`` `...` ``), destructuring, generators, or `async`/`await`. Use `function(){}` and string concatenation.
- No module system — `require`/`import` throw. Capabilities are **globals**, not imports.
- `RegExp` literals (`/\.c$/`) and `JSON` are supported.

Available globals:

| Global | Description |
|--------|-------------|
| `fs.readDir(path)` | List a directory. Returns `[names, errno]` — index `[0]` for the array. |
| `fs.readFile(path)` / `fs.writeFile(path, data)` | Read / write a file (workspace-scoped). |
| `fs.stat(path)` | Returns `[{size, mode, mtime, isDir}, errno]`. |
| `fs.cwd()` | Current working directory. |
| `http_fetch(url)` | Blocking HTTP GET (host-allowlisted). **Throws in channel JS** — channels use `cclaw.send`. |
| `console.log/warn/error` | Buffered; emitted as the result if the script returns `undefined`. |

`js_eval` returns the value of the last expression (or buffered `console.log` output if that is `undefined`). Example: `var f = fs.readDir("."); f[0]`.

## Extension Package Layout

```
workspace/extensions/
├── my_tool.js                    # single-file extension (tools + hooks only)
├── mcp/
│   ├── index.js                  # agent-side: registers MCP tools + hooks
│   └── channel.js                # channel component (optional)
└── discord/
    ├── index.js                  # agent-side: prompt snippets, hooks
    └── channel.js                # channel binary entry point
```

Discovery rules:
1. `workspace/extensions/*.js` — single-file extensions (agent-side only)
2. `workspace/extensions/*/index.js` — package extensions (agent-side component)
3. `workspace/extensions/*/channel.js` — channel component (daemon launches separately)

Load order: alphabetical by filename/dirname.

## Agent-Side Component (Tools + Hooks)

Runs inside `agent_run` in the shared QuickJS context. Loaded fresh each turn (process is disposable — no persistent JS state beyond what's in cclaw.db).

### Factory Function

```javascript
// workspace/extensions/my_extension.js
function(cclaw) {
    // Register tools
    cclaw.registerTool({
        name: "weather",
        description: "Get current weather for a city",
        parameters: '{"type":"object","properties":{"city":{"type":"string"}},"required":["city"]}',
        handler: function(args) {
            var resp = http_fetch("https://wttr.in/" + args.city + "?format=3");
            return resp;
        }
    });

    // Register hooks
    cclaw.registerHook("beforeToolCall", function(ctx) {
        if (ctx.name === "shell_exec" && ctx.args.command.includes("rm -rf")) {
            return { block: true, reason: "destructive command blocked by extension" };
        }
    });

    cclaw.registerHook("beforeRequest", function(messages) {
        // Inject a system message before every LLM call
        messages.unshift({ role: "system", content: "Always be concise." });
        return messages;
    });
}
```

### API Reference

| Method | Description |
|--------|-------------|
| `cclaw.registerTool({name, description, parameters, handler})` | Register a callable tool. `handler(args)` returns string. Delegates to same registry as `js_define_tool`. |
| `cclaw.registerHook(event, fn)` | Register a hook function for an event. |
| `cclaw.callTool(name, args)` | Synchronous call into C tool registry. Returns result string. Re-entrant (depth limit 8). |

### Hook Events

| Event | Signature | Can modify | Notes |
|-------|-----------|-----------|-------|
| `turnStart` | `fn()` | — | Informational. Called at agent_run entry. |
| `beforeRequest` | `fn(messages) → messages\|void` | messages array | Chain in load order. Return modified array or void. |
| `afterResponse` | `fn(response)` | — | Read-only inspect of parsed LLM response. |
| `beforeToolCall` | `fn({name, args}) → {block, reason}\|void` | args (mutate in place) | First `{block:true}` wins. |
| `afterToolCall` | `fn({name, args, result}) → {result}\|void` | result replacement | Chains: each hook sees previous result. |
| `turnEnd` | `fn()` | — | Informational. Called before agent exit. |

### Failure Policy

Extension throws during load or hook execution → skip that extension, log warning to stderr, continue turn. One broken extension ⊥ kill the agent.

### Permissions

Extensions inherit the agent's permissions:
- `allowed_hosts` for `http_fetch`
- Workspace filesystem access
- Shared QuickJS heap (V5: 1MB default, configurable)

No per-extension sandboxing. Extensions are trusted at the same level as `js_define_tool` code.

## Channel Component

A channel extension's `channel.js` runs inside `channel_runner` — a **separate
process** per channel, forked by the daemon (crash isolation, restart with
backoff). The runner owns one single-threaded poll() event loop; channel JS is
purely reactive and **never blocks on the network**. All outbound HTTP is
described as request shapes that the runner's C loop executes on a curl_multi
handle; all inbound HTTP arrives pre-parsed from the daemon.

### Channel Process Lifecycle

```
1. User installs extension (copies to workspace/extensions/<name>/)
2. Agent proposes channel via configure_channel tool (exit code 4)
3. Admin approves → daemon inserts `channels` row + seeds `channel_state`
4. Daemon forks channel process on startup (or immediately after approval)
5. Channel process runs indefinitely (long-poll, proxied webhooks, etc.)
6. On crash: daemon restarts with backoff (max 3 retries);
   outbox rows stuck 'sending' are reset to 'pending' at startup
7. On daemon shutdown: SIGTERM → channel exits
```

### JS Contract (channel.js handlers)

All handlers optional except `onInit`. A request shape is
`{method?, url, body?, headers?: ["Name: value"], timeout?}`.

| Handler | Called when | Returns |
|---------|-------------|---------|
| `onInit()` | startup | `{poll?: Req}` — optional first long-poll shape |
| `onPoll({status, body, error})` | long-poll completed | `{poll?: Req}` next shape; `{poll: null}` stops |
| `onRequest(req)` | daemon proxies an inbound HTTP request over UDS | `{status?, body?}` (or a body string) |
| `onOutbox({id, session_id, payload})` | agent message ready to deliver | nothing — queue sends via `cclaw.send` |
| `onResult({tag, status, body, error})` | a tagged `cclaw.send` completed | nothing |

`onRequest` is where **verification lives** — signature checks, challenge
echoes, auth — because verification is code, not config. The daemon's
`/hook/<channel>` endpoint is a dumb proxy: it forwards
`{method, path, headers, body}` to the runner's unix socket and relays the
reply. Returning `{status: 401}` rejects a forged webhook.

### JS API (`cclaw.*`)

| Function | Description |
|----------|-------------|
| `cclaw.send(req)` | Queue an outbound HTTP request; the C loop executes it. Extra fields: `tag` (get `onResult` callback), `outbox_id` + `final` (auto-ack: the row is acked when the final send gets 2xx, failed otherwise). |
| `cclaw.emit(type, payload_json)` | Insert `channel_events` row + `daemon_wake()`. Daemon routes to agent inbox. |
| `cclaw.getConfig(key)` / `setConfig(key, value)` | `channel_state` kv (scoped to this channel). |
| `cclaw.ackOutbox(id)` / `failOutbox(id, error)` | Manual outbox resolution (e.g. unparseable payload). Sends with `outbox_id` ack automatically. |
| `cclaw.log(msg)` | stderr line, prefixed with channel name. |
| `cclaw.admin.*` | Admin operations (keys, models, hosts) for admin-gated chat commands. |

There is **no blocking fetch**. `http_fetch` throws in channel JS.

### Channel Event Flow

```
Incoming (platform → agent), webhook mode:
  platform POSTs https://daemon/hook/<channel>
    → daemon web server forwards envelope over <db>.<channel>.sock
    → runner: onRequest(req) verifies, parses, cclaw.emit(...)
    → reply {status, body} relayed to the platform by the daemon
    → daemon reads channel_events, routes to agent inbox, forks agent

Incoming (platform → agent), long-poll mode:
  C loop completes the poll shape
    → onPoll(result) parses, cclaw.emit(...), returns next shape

Outgoing (agent → channel):
  agent completes turn (exit 0)
    → daemon INSERT channel_outbox (status='pending') + FIFO wake
    → runner marks row 'sending', calls onOutbox(item)
    → onOutbox queues cclaw.send(..., {outbox_id: item.id, final: 1})
    → C loop executes sends in order; final 2xx → 'delivered',
      any failure → 'failed: <err>' and queued sends for that row dropped
```

### Example: minimal webhook channel

```javascript
function onInit() { return {}; }   // no polling; webhook only

function onRequest(req) {
    if ((req.headers["X-Hub-Signature"] || "") !== expected(req.body))
        return {status: 401, body: "bad signature"};
    var ev = JSON.parse(req.body);
    cclaw.emit("message", JSON.stringify({channel_id: ev.chat, text: ev.text}));
    return {status: 200, body: "ok"};
}

function onOutbox(item) {
    var p = JSON.parse(item.payload);
    cclaw.send({url: API + "/send", body: JSON.stringify({to: p.channel_id, text: p.text}),
                outbox_id: item.id, final: 1});
}
```

## Schema (cclaw.db additions)

```sql
CREATE TABLE channels (
    name        TEXT PRIMARY KEY,
    type        TEXT NOT NULL,           -- 'telegram', 'discord', 'webhook', etc.
    binary_path TEXT NOT NULL,           -- path to channel binary/script
    status      TEXT DEFAULT 'active',   -- active|failed|disabled
    pid         INTEGER,                 -- current process pid (NULL if not running)
    created_at  INTEGER DEFAULT (unixepoch())
);

CREATE TABLE channel_events (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    channel_name TEXT NOT NULL,
    event_type   TEXT NOT NULL DEFAULT 'message',
    payload      TEXT NOT NULL,
    created_at   INTEGER DEFAULT (unixepoch())
);

CREATE TABLE channel_outbox (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    channel_name TEXT NOT NULL,
    session_id   INTEGER NOT NULL,
    payload      TEXT NOT NULL,
    status       TEXT DEFAULT 'pending',  -- pending|sending|delivered|failed
    error        TEXT,
    created_at   INTEGER DEFAULT (unixepoch()),
    acked_at     INTEGER
);
CREATE INDEX idx_outbox_pending ON channel_outbox(channel_name, status)
    WHERE status = 'pending';

CREATE TABLE channel_state (
    channel_name TEXT NOT NULL,
    key          TEXT NOT NULL,
    value        TEXT,
    PRIMARY KEY (channel_name, key)
);
```

## Relationship to Existing Systems

| Before | After |
|--------|-------|
| Telegram runs as thread inside daemon | Telegram runs as channel process (same as extensions) |
| `daemon_signal_external` writes SignalMsg to FIFO | `daemon_wake()` writes 1 byte; daemon scans `channel_events` |
| `deliver_response` calls `telegram_send_message` directly | Daemon writes `channel_outbox`; channel process delivers |
| `channel_bindings` maps type→agent | Still used — daemon reads to route `channel_events` to correct agent |
| Internal signal_pipe for in-process wake | Unchanged — cron, spawn_queue still use it |

## Installation Flow

1. Agent creates extension files in `workspace/extensions/<name>/`
2. Agent-side component (index.js) loads automatically next turn
3. For channel component: agent calls `configure_channel` tool with `{type, binary_path, config: {key: value, ...}}`
4. Admin approves (V54)
5. Daemon inserts `channels` row, seeds `channel_state` with config, launches process

## Design Decisions

**Why one package for both?** An extension like MCP or Discord needs agent-side behavior (prompt guidance, tool registration) AND a channel process (network I/O, protocol handling). Keeping them together means one install, one update, coherent versioning.

**Why separate processes for channels?** Channels have unbounded lifetimes and external network dependencies. A misbehaving Discord gateway reconnect loop shouldn't affect agent turns or the daemon's epoll loop. Crash isolation is the primary motivation.

**Why shared context for agent-side?** Extensions need to interact — an MCP extension registers tools that a guardrails extension might hook via `beforeToolCall`. Separate contexts would prevent this. The shared heap is bounded by V5 limits regardless.

**Why no persistent JS state?** Agent processes are one-turn-then-exit. Any state that needs to survive goes in cclaw.db (via `callTool("db_query", ...)` or the existing `js_define_tool` persistence path). This matches CClaw's process model — memory fully reclaimed after each turn.
