---
name: cclaw-channels
description: How CClaw channels work — the channel extension component, activation via config keys, routing gate, delivery modes, the inbox/outbox flow, and the QuickJS channel API.
---

# CClaw Channels

A channel connects CClaw to an outside surface (Telegram, a webhook, a custom
feed). Channels are shipped as the `channel` component of an extension: a
QuickJS handler file plus config keys, run by the daemon in a dedicated
runner process (`cclaw --channel <name>`).

## Anatomy

- `channels` table row: channel name → `extension_name`; `status` is its trust
  state (draft → active).
- Config: namespaced registry keys `<extension>.<key>`. Convention:
  `enabled` (launch switch), `base_url` (egress pin — the runner refuses sends
  to any other host), plus whatever the handler needs. Secret keys (tokens)
  resolve from env / the encrypted secret store only.
- State: `channel_state` rows are the channel's private runtime scratch
  (poll offsets, feature latches) via `getState`/`setState`.

## Launch gate

The daemon launches a channel only when **all** hold:

1. trust status is `active` (promotion / operator activation),
2. `<ext>.enabled` resolves truthy,
3. every config key flagged `required` resolves non-empty.

So "turn on telegram" = ensure `telegram.bot_token` is provided (env
`CCLAW_TELEGRAM_BOT_TOKEN` or secret store) and set `telegram.enabled=1`.

## Routing gate

Messages from unknown senders are **dropped by default** — there is no
unconditional `default_agent` fallback. The routing gate
(`channel_consume_events`) evaluates each inbound event in order:

1. **Routed** — exact `(channel, channel_id)` hit in `channel_routes`, or
   the `(channel, '*')` wildcard → deliver to the route's agent/session.
2. **Unrouted, admin** — sender's `channel_id` is in `<ext>.admin_ids` →
   accept via `default_agent`, auto-creating a session (the operator always
   gets through).
3. **Unrouted, allow_unknown** — `<ext>.allow_unknown=1` → accept via
   `default_agent` (the pre-gate open behavior; off by default).
4. **Unrouted, unknown** — drop the message + log + a **one-time admin
   notification** carrying the sender id/name and the `cclaw route add`
   recipe so the operator can opt them in.

Chat membership is never authority. `admin_ids` is the only admin source.

### Adding routes

```
cclaw route add <channel> <chat_id> <agent> [--mode auto|explicit] [--session <id>] [--tools name,name,...]
cclaw route rm  <channel> <chat_id>
cclaw route list
```

Group-shaped (negative) chat ids default to `--mode explicit`.
`--tools` attaches a tool filter to the route (see Authority attenuation below).

A route with a `--session <id>` **pins** that chat to a specific session.
Otherwise: find-latest session for `(channel_name, channel_id)`, create one
under the route's agent if none exists.

### Authority attenuation

A route may carry a **tool filter** — a JSON array of tool names stored in
`channel_routes.tool_filter` (NULL = unrestricted). When a channel message
creates a new session for a routed sender, the route's filter is copied onto
`sessions.tool_filter`, frozen at session creation (same semantics as
sub-agent spawns).

Each turn, the effective tool set is **grants ∩ filter** — the filter can
only shrink authority, never widen it. Filtering to a tool the agent isn't
granted exposes nothing: the intersection is empty.

Changing a route's filter later does **not** propagate to existing sessions;
a new session is needed to pick up the new filter. Unrouted admin-accepted
senders get no filter (full grant set applies).

`route list` displays the active filter per route. Example:

```
cclaw route add telegram 12345 researcher --tools web_fetch,js_eval
cclaw route list
# telegram 12345 -> researcher (mode auto, tools ["web_fetch","js_eval"])
```

## Delivery modes

`channel_routes.delivery_mode` controls how the agent's output reaches the
chat:

- **`auto`** (DM default): assistant turn output is inserted into
  `channel_outbox` for the origin chat automatically.
- **`explicit`** (default for group-shaped ids): turn output stays in the
  session; the agent speaks only via the `channel_send` tool. This is the
  group listen-and-decide pattern — `mentioned` / `reply_to_me` envelope
  fields are the engage signals; silence is the default.

Unrouted flows (admin / allow_unknown) use `auto`.

## Envelope (handler → C)

The handler calls `channel.emit("message", envelope_json [, external_id])`
with a JSON payload containing these fields (all but `channel_id` optional):

| Field | Meaning |
|-------|---------|
| `channel_id` | Chat/conversation id — the routing key |
| `text` | Message text (or media caption) |
| `sender_id` | Platform sender (in a DM usually == channel_id; in a group the person) |
| `sender_name` | Display name, attribution only |
| `chat_type` | `dm` or `group` |
| `mentioned` | bool — the bot was @-mentioned |
| `reply_to_me` | bool — this message replies to one of ours |

The handler reports platform **facts only** — it never makes authority
decisions. The raw envelope JSON never reaches the model: C extracts plain
text for the session entry (bare `text` for DMs; `"<sender_name>: <text>"`
for groups where attribution is load-bearing in explicit mode).

## channel_send tool

The agent can proactively send messages via the `channel_send` tool:

- Schema: `{channel, chat_id, message}` (text-only v1).
- `action: "list"` enumerates reachable targets.
- **Routes are the allowlist (default-deny):** a target needs an exact
  `(channel, chat_id)` route resolving to the sending agent. A `'*'` wildcard
  route grants no send authority — reaching a new target requires
  `cclaw route add`.
- Delivery is async: the tool inserts an outbox row and returns
  `queued, outbox id N`.
- Sends to the session's own origin chat in `auto` mode short-circuit (the
  reply already delivers there).
- Ships **ungranted** — the operator grants it per agent.

## Ingestion-only channels

A handler that defines no `onOutbox` is a pure source. The runner records
`channel_state.has_outbox='0'` at load time, and the daemon skips the
per-turn outbox insert for its sessions (otherwise rows would sit pending
forever). Absent key = assume duplex.

## Handler contract (channel.qjs)

The handler exports lifecycle functions; the C runner owns the loop, HTTP, and
retries:

- `onInit()` — read config, return `{poll: {method,url,…}}` to start polling.
- `onPoll(result)` — parse the poll response, `channel.emit('message', json)`
  for each inbound item, return the next poll shape.
- `onOutbox(item)` — deliver one outbound item: build the send with
  `channel.send({method,url,body, outbox_id: item.id, final: 1})`.
- `onRequest(req)` — handle webhook/UDS HTTP requests (verification,
  signature checks); return `{status, body}`.

Host API: `channel.emit`, `channel.send`, `channel.http`,
`channel.getConfig(key)` (registry, read-only),
`channel.getState/setState(key, value)` (runtime scratch),
`channel.log`, `channel.ackOutbox/failOutbox`,
and `channel.admin.isAdmin(id)` / `channel.admin.listPendingApprovals()` /
`channel.admin.dashboardUrl()`.

## Message flow

```
Inbound:
  handler emit → channel_events → daemon routing gate → inbox → session entry
Outbound (auto mode):
  agent reply → channel_outbox → runner onOutbox → channel.send → platform
Outbound (explicit mode):
  agent calls channel_send tool → channel_outbox → runner onOutbox → platform
```
