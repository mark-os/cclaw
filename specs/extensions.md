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

Runs inside `agent_run` in the shared QuickJS context. Loaded fresh each turn (process is disposable — no persistent JS state beyond what's in agent.db).

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

A channel extension's `channel.js` (or compiled binary) runs as a **separate process** managed by the daemon. It communicates exclusively through the channel API.

### Channel Process Lifecycle

```
1. User installs extension (copies to workspace/extensions/<name>/)
2. Agent proposes channel via configure_channel tool (exit code 4)
3. Admin approves → daemon inserts `channels` row + seeds `channel_state`
4. Daemon forks channel process on startup (or immediately after approval)
5. Channel process runs indefinitely (polling, webhooks, etc.)
6. On crash: daemon restarts with backoff (max 3 retries)
7. On daemon shutdown: SIGTERM → channel flushes outbox → exits
```

### Channel API

Channel processes link against `libchannel_api` (or use the mjs binary with channel bindings). Limited cclaw.db access — no arbitrary SQL.

| Function | Description |
|----------|-------------|
| `channel_emit(ctx, payload_json)` | Insert `channel_events` row + `daemon_wake()`. Daemon routes to agent inbox. |
| `channel_get_config(ctx, key)` | Read from `channel_state` kv (scoped to this channel). |
| `channel_set_config(ctx, key, value)` | Write to `channel_state` kv (own state only). |
| `channel_next_outbox(ctx)` | Return oldest pending outbox row for this channel. Blocking or poll. |
| `channel_ack_outbox(ctx, id)` | Mark outbox row as delivered. |
| `channel_fail_outbox(ctx, id, error)` | Mark outbox row as failed with error message. |

### Channel Event Flow

```
Incoming (channel → agent):
  channel_emit(ctx, '{"chat_id":123,"text":"hello","from":"user"}')
    → INSERT channel_events (channel_name, event_type='message', payload)
    → daemon_wake() (1 byte to FIFO)
    → daemon reads channel_events
    → daemon resolves agent via channel_bindings
    → daemon_inbox_insert(agent_name, session_id, source, payload)
    → fork agent

Outgoing (agent → channel):
  agent completes turn (exit 0)
    → daemon reaps, reads response
    → daemon INSERT channel_outbox (channel_name, session_id, payload)
    → channel process: channel_next_outbox(ctx) returns row
    → channel delivers (sendMessage, webhook POST, etc.)
    → channel_ack_outbox(ctx, id)
```

### Example: Discord Channel Extension

```
workspace/extensions/discord/
├── index.js       # agent-side: adds prompt guidance for Discord formatting
└── channel.js     # channel process: Discord gateway + outbox delivery
```

**index.js** (runs in agent_run):
```javascript
function(cclaw) {
    cclaw.registerHook("beforeRequest", function(messages) {
        // Add Discord-specific formatting guidance
        messages.push({
            role: "system",
            content: "Format responses for Discord: use markdown, keep under 2000 chars."
        });
        return messages;
    });
}
```

**channel.js** (runs as separate process via mjs):
```javascript
// Launched by daemon as: mjs workspace/extensions/discord/channel.js
var token = channel_get_config("bot_token");
// ... Discord gateway connection, message polling ...
// On message received:
channel_emit(JSON.stringify({ guild_id: "...", channel_id: "...", text: msg.content }));
// Outbox delivery loop:
while (true) {
    var item = channel_next_outbox();
    if (item) {
        discord_send(item.payload);
        channel_ack_outbox(item.id);
    }
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
    status       TEXT DEFAULT 'pending',  -- pending|delivered|failed
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

**Why no persistent JS state?** Agent processes are one-turn-then-exit. Any state that needs to survive goes in agent.db (via `callTool("db_query", ...)` or the existing `js_define_tool` persistence path). This matches CClaw's process model — memory fully reclaimed after each turn.
