# Extension System

## Overview

An **extension** is a bundle of related capabilities: a manifest plus the handler
files it points to. It can provide any combination of the following — zero or more
of each:

| Component | Runs in | Lifecycle |
|-----------|---------|-----------|
| Tools | forked child of the agent process (one fork+exec per call) | per-call, isolated |
| Hooks | agent process, fresh QuickJS context per dispatch | per turn-event |
| Channel | separate `cclaw --channel` process (daemon-managed) | long-lived, restarted on crash |
| Scripts | forked child, on a cron schedule | per-run, isolated |
| Skills (future) | — | — |

The extension is the **unit** — the thing you install, enable, disable, uninstall,
and publish. Every capability it provides carries its `extension_name`, so the set
is always known and removable as a whole. This is the property the earlier
implementation lacked: tools were discovered by *executing* JS that called
`cclaw.registerTool()` and scraping the resulting globals, so nothing remembered
which extension a tool came from and an extension had no runtime identity.

**The manifest declares the contents; nothing registers itself by running.** See
[Why declarative, not `registerTool`](#why-declarative-not-registertool).

## The Manifest

Each extension is a directory whose root holds `extension.json`. The manifest
*declares* what the extension provides; handler paths are relative to the bundle
directory.

```json
{
  "name": "nws",
  "version": "0.1.0",
  "description": "US National Weather Service tools",

  "tools": [
    {
      "name": "get_forecast",
      "description": "Get the NWS forecast for a lat/lon",
      "parameters": { "type": "object",
        "properties": { "lat": {"type":"number"}, "lon": {"type":"number"} },
        "required": ["lat","lon"] },
      "handler": "forecast.js",
      "policy": null
    }
  ],

  "hooks": [
    { "event": "beforeToolCall", "handler": "guard.js" }
  ],

  "channel": { "type": "telegram", "handler": "channel.js" },

  "scripts": [
    { "name": "morning_report", "handler": "report.js", "schedule": "0 7 * * *" }
  ]
}
```

| Field | Meaning |
|-------|---------|
| `name` | Extension identity. Primary key in `extensions`. |
| `version` | Free-form version string (display / update tracking). |
| `tools[]` | Each: `{name, description, parameters (JSON Schema object), handler (file), policy?}`. |
| `hooks[]` | Each: `{event, handler (file)}`. `event` is one of the six hook events below. |
| `channel` | At most one: `{type, handler (file)}`. The channel runs as a separate process. |
| `scripts[]` | Each: `{name, handler (file), schedule? (cron expr)}`. A schedule seeds a `cron` row. |

The manifest carries **identity and declarations only** — never tool *code*. Code
lives in the handler files. (DB holds config + path; files hold code.)

## Two Locations: Draft vs. Installed

Extensions exist in one of two places, and which one a file is in *is* its trust
state:

```
~/.cclaw/agents/<agent>/workspace/extensions/<name>/   ← DRAFT  (agent-writable, private)
~/.cclaw/extensions/<name>/                             ← INSTALLED (agent-immutable, shared)
```

- A **draft** is the authoring working copy. The owning agent can read and write
  it; it is visible only to that agent. Drafts are discovered by scanning the
  workspace.
- An **installed** extension lives in the shared store outside any workspace. Tool
  children get this directory mounted **read-only**, so a registered extension's
  code cannot be modified by the agent that owns it. The DB's `extensions.path`
  points here.

"Migrating to `~/.cclaw`" means exactly this: **promotion copies the bundle from
the workspace into the shared store, and ingests its declarations into the DB.**
(File copy, not a schema migration — no conflict with the no-migrations rule.)

## Lifecycle

Four verbs, each a DB operation plus (for promote) a file copy:

| Verb | Effect | DB | Files |
|------|--------|-----|-------|
| **draft** | author/iterate, usable by owner only | — | write `workspace/extensions/<name>/` |
| **promote** | register as a real extension for the owner | `INSERT extensions(owner_agent,…,published=0)`, ingest `tools`/`hooks` rows, `INSERT agent_extensions(self)`, seed `cron` for scheduled scripts | **copy** bundle → `~/.cclaw/extensions/<name>/` |
| **publish** | make attachable by any agent | `UPDATE extensions SET published=1` | — |
| **attach** | another agent installs a published extension | `INSERT agent_extensions(other, name)` | — |

Inverse operations are pure SQL: **disable** = `UPDATE agent_extensions SET
enabled=0`; **uninstall** = `DELETE FROM agent_extensions WHERE extension_name=?`
(the load join stops returning its capabilities); fully removing a published
extension also deletes its `extensions`/`tools`/`hooks` rows and the shared-store
directory.

Promotion is the **trust boundary** (per [security.md](security.md)): a sub-agent's
draft must not auto-promote into a globally callable tool. Because promote copies
the code out of the writable workspace, a tool that has been promoted is immutable
from the agent's side — closing the promote-then-swap-the-handler hole.

## Loading: the DB is the registry

The in-memory tool registry is a **cache of a SQL query**, not a parallel truth.
At agent setup, the registry is materialized for the current agent:

```sql
SELECT t.name, t.description, t.parameters_json, t.path, t.policy
  FROM tools t
  JOIN agent_extensions ae ON ae.extension_name = t.extension_name
 WHERE ae.agent_name = ?1 AND ae.enabled = 1
   AND (e.published = 1 OR e.owner_agent = ?1);
```

Built-in C tools register as today; extension tools come from this join. Hooks load
the same way from the `hooks` table. **No JS is evaluated at load time** — loading
is parsing JSON-derived rows, not running extension code. The only JS execution is
when a tool *runs* (fork child) or a hook *fires* (fresh context). This deletes the
entire eval-and-scrape subsystem (`__cclaw_api`, `__cclaw_tools[]` accumulation,
`process_registered_tools`/`process_registered_hooks`).

Because the registry is derived from the DB, a tool promoted **mid-session** becomes
callable the same session by re-running the query and appending to the live
registry — no restart, no parallel in-memory state. This mirrors
`agent_setup_refresh_caps()`, which already re-reads grants and rebinds live tool
contexts.

## Tool Handler Contract

A tool handler is a JS file. It runs in a **forked, sandboxed child** (`cclaw
--qjs_eval`) — one fork+exec per call, no shared process state. The child wraps the
handler file as a **function body** with the call's `args` (a parsed object) in
scope — roughly `(function(args){ <file> })(args)` — so the handler must `return`
its result (a bare trailing expression evaluates to `undefined`). The returned
value is the tool result string; if it returns `undefined`/nothing, buffered
`console.log` output is used instead. This is the same execution model as the
`js_eval` tool in file mode, so there is one JS contract, not two.

Because handlers fork+exec, **a handler cannot capture closures or share an
in-process client** — every input is static data and every run is isolated. This is
precisely why declarative manifests lose nothing relative to imperative
registration (see design notes).

### JS dialect & globals

JavaScript runs in **QuickJS** (bellard/quickjs). The eval/channel profiles have
full modern JS (`let`/`const`, arrow functions, template literals, `async`/`await`,
`Promise`); the in-process hooks profile is minimal (Base + Eval + JSON + RegExp,
no `Promise`). Globals available to tool handlers (the `qjs_register_eval_host_functions`
environment):

| Global | Description |
|--------|-------------|
| `http_request(req)` | Blocking HTTP (host-allowlisted). Returns `{status, body}`. External bodies are boundary-wrapped. |
| `fs.readFile/writeFile/readdir/stat/lstat/cwd` (+ `…Sync` aliases) | Workspace-scoped filesystem. |
| `console.log/warn/error`, `print` | Buffered; emitted as the result when the handler returns `undefined`. |

Secrets are referenced as `{{SECRET:NAME}}` in handler arguments and interpolated
by C before execution — never visible to the model or written to the handler.

## Hook Handler Contract

A hook handler is a JS file run **in the agent process** in a fresh QuickJS context
per dispatch (isolation between extensions; no cross-call leakage). The handler
receives a small structured `input` object (never the full messages array) and
returns a JSON object of **commands** that C validates and applies — see
[specs/hooks.md](hooks.md) for the full command contracts.

| Event | Signature | Commands honored | Notes |
|-------|-----------|-----------------|-------|
| `preAdvance` | `fn(input) → cmds\|void` | `inject` (system/user, ephemeral or persistent, capped), `suppress` (this request only; never the latest user entry), `pin` (survives the context cut), `set_data` (json_patch onto `entries.data`) | Chained in load order; commands accumulate. Pin wins over suppress. |
| `postAdvance` | `fn(input) → cmds\|void` | `transform_content` (rewrites the assistant entry; first original kept in `data.original_content`), `annotate` (merges into `data`) | Chained; later hooks see the transformed `input.content`. Last transform wins, annotate merges. |
| `beforeToolCall` | `fn({name, args}) → {deny\|ask, reason}\|void` | restrict-only gate | Most-restrictive across hooks wins; a hook can veto, never authorize. |
| `afterToolCall` | `fn({name, args, result}) → {result, annotate}\|void` | result replacement + `annotate` | Chained; each sees the previous result. Runs after the secret scanner, before the entry write. |
| `turnStart` / `turnEnd` | `fn()` | — | Declared, not yet dispatched. |

Deferred for now: `inject_next` and `notify` (postAdvance commands), and the
`db.query`/`kv`/`console.log` hook globals — hooks act on their `input` JSON only.

A hook that throws during dispatch is skipped (logged); one broken hook must not
kill the turn.

Caveat: `suppress` is not sequence-checked — hiding an individual `tool_result`
entry while its `tool_call` stays in context can produce a provider-invalid
message sequence. Suppress whole turns, not single tool results.

## Channel Component

A channel runs as `cclaw --channel <name>` — a **separate process per channel**,
fork+exec'd by the daemon (clean process image, crash isolation, restart with
backoff); there is no separate runner binary. The runner owns one
single-threaded `poll()` event loop; channel JS is purely reactive and **never
blocks on the network**. All outbound HTTP is described as request shapes the
runner's C loop executes on a `curl_multi` handle; inbound HTTP arrives pre-parsed.

The channel is declared in the manifest (`"channel": {type, handler}`); promoting an
extension with a channel inserts the `channels` row (joined to the extension by
`extension_name`) and the daemon launches `<ext_path>/<handler>`.

### JS contract (channel handler)

All handlers optional except `onInit`. A request shape is
`{method?, url, body?, headers?: ["Name: value"], timeout?}`.

| Handler | Called when | Returns |
|---------|-------------|---------|
| `onInit()` | startup | `{poll?: Req}` — optional first long-poll shape |
| `onPoll({status, body, error})` | long-poll completed | `{poll?: Req}` next shape; `{poll: null}` stops |
| `onRequest(req)` | daemon proxies an inbound HTTP request over UDS | `{status?, body?}` (or a body string) |
| `onOutbox({id, session_id, payload})` | agent message ready to deliver | nothing — send via `cclaw.send` |
| `onResult({tag, status, body, error})` | a tagged `cclaw.send` completed | nothing |

`onRequest` is where **verification lives** — signature checks, challenge echoes,
auth — because verification is code, not config. The daemon's `/hook/<channel>`
endpoint is a dumb proxy. Returning `{status: 401}` rejects a forged webhook.

### Channel JS API (`cclaw.*`)

| Function | Description |
|----------|-------------|
| `cclaw.send(req)` | Queue an outbound request. Extra: `tag` (→ `onResult`), `outbox_id` + `final` (auto-ack the row when the final send is 2xx). |
| `cclaw.emit(type, payload_json)` | Insert `channel_events` + `daemon_wake()`. Daemon routes to the agent inbox. |
| `cclaw.getConfig(key)` / `setConfig(key, value)` | `channel_state` kv (scoped to this channel). |
| `cclaw.ackOutbox(id)` / `failOutbox(id, error)` | Manual outbox resolution. |
| `cclaw.log(msg)` | stderr line, prefixed with channel name. |
| `cclaw.admin.*` | Admin operations (keys, models, hosts) for admin-gated chat commands. |

There is **no blocking fetch** in channel JS; `http_request` is unavailable —
channels use `cclaw.send`.

### Event flow

```
Incoming (webhook):
  platform POSTs https://daemon/hook/<channel>
    → daemon forwards envelope over <db>.<channel>.sock
    → runner: onRequest(req) verifies, parses, cclaw.emit(...)
    → reply {status, body} relayed to the platform
    → daemon reads channel_events, routes to agent inbox, forks agent
Incoming (long-poll):
  C loop completes the poll shape → onPoll(result) → cclaw.emit(...) → next shape
Outgoing:
  agent turn completes → daemon INSERT channel_outbox(pending) + FIFO wake
    → runner marks 'sending', onOutbox(item) → cclaw.send(..., {outbox_id, final:1})
    → C loop sends in order; final 2xx → 'delivered', any failure → 'failed'
```

## Scripts

A script is a JS handler run as a forked child on a schedule — the low-ceremony
path. The canonical example: *"send me a weather report every morning."* The agent
writes `report.js`, and either runs it ad hoc or declares `"schedule": "0 7 * * *"`
so promotion seeds a `cron` row. No tool, no channel, no promotion ladder required
for a one-off. An extension exists only when the user wants a reusable, named,
publishable capability (an `nws` extension with a `get_forecast` tool) rather than a
single scheduled script.

## Schema (cclaw.db)

Deltas to the current schema; channel tables (`channel_events`, `channel_outbox`,
`channel_state`, `channel_routes`) are unchanged — see [schema.md](schema.md).

```sql
-- extension = the installable unit
CREATE TABLE extensions (
  name        TEXT PRIMARY KEY,
  path        TEXT NOT NULL,            -- shared store dir: ~/.cclaw/extensions/<name>
  version     TEXT DEFAULT '0.0.0',
  owner_agent TEXT,                     -- who promoted it (NULL for builtin)
  published   INTEGER NOT NULL DEFAULT 0,  -- the single publish flag
  builtin     INTEGER NOT NULL DEFAULT 0,
  enabled     INTEGER NOT NULL DEFAULT 1,
  created_at  INTEGER NOT NULL DEFAULT (unixepoch())
);

-- which agents have installed which extensions (+ per-agent config)
CREATE TABLE agent_extensions (
  agent_name     TEXT,
  extension_name TEXT NOT NULL,
  config         TEXT,
  enabled        INTEGER NOT NULL DEFAULT 1,
  PRIMARY KEY (agent_name, extension_name)
);

-- tools: authoritative registry, provenance via extension_name
CREATE TABLE tools (
  name            TEXT PRIMARY KEY,
  extension_name  TEXT,                 -- NULL for built-in C tools
  description     TEXT,
  parameters_json TEXT,
  path            TEXT,                 -- handler file (in the shared store)
  agent_name      TEXT,                 -- owner scope, NULL = global
  policy          TEXT,
  builtin         INTEGER NOT NULL DEFAULT 0,
  enabled         INTEGER NOT NULL DEFAULT 1
);

-- hooks: same provenance model as tools (replaces the __cclaw_hooks scrape)
CREATE TABLE hooks (
  extension_name TEXT NOT NULL,
  event          TEXT NOT NULL,         -- one of the six hook events
  path           TEXT NOT NULL,         -- handler file (in the shared store)
  enabled        INTEGER NOT NULL DEFAULT 1,
  PRIMARY KEY (extension_name, event, path)
);
```

The `tools` table becomes the source of truth (it was previously written by
`tools_sync_to_db` with `builtin=1` and no path — a display mirror). Loading reads
it; promote writes it.

## Security & Trust

- **Promotion is the trust boundary.** Drafts are private and untrusted; an
  installed extension's code is copied out of the agent-writable workspace so it is
  immutable from the agent's side. No promote-then-swap.
- **One read-only mount.** Tool children get `~/.cclaw/extensions/` mounted
  read-only — a single mount serves every agent, instead of cross-agent workspace
  mounts that publishing-from-workspace would require.
- **Trust level unchanged.** Extension tools run at the same sandbox trust as the
  agent's other tools (`agents.trust_level`); they get no extra privilege. Per-tool
  `policy` and `grants` gate *callability* — a statically-declared tool can still be
  approval-gated or denied. Conditional availability is a policy concern, not a
  registration-time concern.

## Design Decisions

### Why declarative, not `registerTool`

`registerTool`/`registerHook` (code that registers itself at load) earns its keep in
in-process plugin systems where a handler is a live closure over a configured
client and registration can be conditional. **CClaw discarded that model when it
chose fork+exec isolation:** a handler is an isolated source string, so every
`registerTool` argument is already static data — the call is a JSON literal wearing
a function-call costume. The only things imperative registration buys here are
*conditional* and *programmatic* registration, and both undermine the goal of an
inspectable, DB-authoritative registry (a tool that only sometimes exists depending
on load-time JS). Programmatic generation is better done by generating the manifest
once at author time; conditional availability is a `policy`/`grants` concern. So the
manifest declares; nothing registers by running. This also realigns with the
documented intent that defining a tool is *"a path to a code file plus the tool's
schema — never inline JS."*

### Why a shared store instead of staying in the workspace

A published extension must be readable by other agents and immutable by its author.
Keeping it in the owner's writable workspace breaks both (cross-tenant reads;
mutable-after-promote). A central file store beside the central DB registry is the
in-grain choice. The alternative — copying the bundle into each consumer's
workspace on attach — buys workspace portability at the cost of duplication and
frozen copies (no update propagation); rejected for CClaw because the DB is already
the central registry.

### Why separate processes for channels

Channels have unbounded lifetimes and external network dependencies. A misbehaving
gateway reconnect loop must not affect agent turns or the daemon loop. Crash
isolation is the primary motivation.

### Why fresh context per hook, forked child per tool

Hooks run in-process for low latency but in a fresh QuickJS context each dispatch,
so one extension's hook cannot pollute another's (QuickJS has no safe context-reset
primitive; a fresh context *is* the reset). Tools fork+exec for full sandbox
isolation and to bound a misbehaving handler. Both keep the long-lived runtime's GC
heap shared and reused — one runtime, many short-lived contexts.

## Implementation Status

This spec describes the **target** model. As of this writing the codebase still
loads extensions via the `cclaw.registerTool`/`registerHook` scrape
(`src/extension.c`, `CCLAW_API_INIT`) and `tools_sync_to_db` writes a display-only
mirror. The **channel component contract above is implemented** (`cclaw --channel`,
the `channels`/`channel_*` tables, the `cclaw.*` channel API). Migrating tools/hooks
to the declarative model is the work this spec defines: the manifest loader, the
shared store + promote copy, the `tools`/`hooks`/`extensions` schema deltas, the
read-only mount, and the promote/publish/attach operations.
