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

1. **Routed** — exact `(channel, chat_id)` hit in `channel_routes` → deliver
   to the pinned session (the session names its agent).
2. **Unrouted, open door** — the channel's `default_agent` is set → accept:
   a session is created for that agent and the chat is pinned to it.
3. **Unrouted, admin** — the chat is in `<ext>.admin_ids` → accept via the
   global `default_agent` config (the operator always gets through), same
   create-and-pin.
4. **Unrouted, unknown** — drop the message + log + a **one-time admin
   notification** carrying the sender id/name and the `cclaw route add`
   recipe so the operator can opt them in.

Admin chats can also issue `/new` (re-point the chat at a fresh session)
and `/sessions [id]` (list this chat's sessions / attach to one) — handled
in C before dispatch, so they work even when the current session is stuck.

They can also decide approvals from chat: `/approvals` lists what is parked
(id, agent, tool, arguments), `/approve <id>` and `/deny <id>` settle one, and
`/status` shows the chat's session, agent, model and pending count. An
approval can only be decided from the channel it was raised on, and only while
it is still pending; who decided is recorded in `approvals.decided_via`.

Chat membership is never authority. `admin_ids` is the only admin source.

### Adding routes

```
cclaw route add <channel> <chat_id> <agent> [--mode auto|explicit] [--session <id>] [--tools name,name,...]
cclaw route rm  <channel> <chat_id>
cclaw route list
```

A route **pins the chat to a session**: `add` creates the session (bound to
`<agent>`) unless `--session <id>` pins an existing one — the session must
belong to the named agent. Group-shaped (negative) chat ids default to
`--mode explicit`. `--tools` freezes a tool filter onto the created session
(see Authority attenuation below).

`chat_id '*'` is not a route — it sets/clears the channel's `default_agent`
(open-door policy above). `--tools` with `'*'` sets the channel's default
filter for unrouted chats (see below); `--mode`/`--session` don't apply.

### Authority attenuation

A route may carry a **tool filter** — a JSON array of tool names stored in
`channel_routes.tool_filter` (NULL = unrestricted). `route add --tools`
freezes the filter onto the session it creates (`sessions.tool_filter`),
same semantics as sub-agent spawns.

Each turn, the effective tool set is **grants ∩ filter** — the filter can
only shrink authority, never widen it. Filtering to a tool the agent isn't
granted exposes nothing: the intersection is empty.

Changing a route's filter later does **not** propagate to the pinned
session; re-run `route add` (a fresh session) to apply a new filter.
Gate-created sessions (open-door / admin acceptance) freeze the channel's
`default_tool_filter` — `cclaw route add <channel> '*' <agent> --tools
name,name`. NULL (the default) means no filter, i.e. the full grant set.
Admins are not exempt: an unrouted chat is an unrouted chat, so admin power
comes from the explicitly routed admin channel, not from `admin_ids`.

`route list` displays the active filter per route. Example:

```
cclaw route add telegram 12345 researcher --tools web_fetch,js_eval
cclaw route list
# telegram 12345 -> session 7 (researcher, mode auto, tools ["web_fetch","js_eval"])
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

Sessions created by the gate (open-door / admin acceptance) start `auto` —
the pin row is written with the default mode.

## Envelope (handler → C)

The handler calls `channel.emit("message", envelope_json [, external_id])`
with a JSON payload containing these fields (all but `chat_id` optional):

| Field | Meaning |
|-------|---------|
| `chat_id` | Chat/conversation id — the routing key |
| `text` | Message text (or media caption) |
| `sender_id` | Platform sender (in a DM usually == chat_id; in a group the person) |
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
- **Routes are the allowlist (default-deny):** a target needs a
  `(channel, chat_id)` route whose pinned session belongs to the sending
  agent. A channel `default_agent` grants no send authority — reaching a
  new target requires `cclaw route add`.
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
  A payload with `to_user: 1` (admin notices, approval prompts fanned out to
  `admin_ids`) addresses a **user** id, not a chat id. Where the platform
  distinguishes the two the handler resolves it — Discord opens a DM channel
  via `POST /users/@me/channels` and caches the result; Telegram needs
  nothing, a DM's chat id *is* the user id.
- `onRequest(req)` — handle webhook/UDS HTTP requests (verification,
  signature checks); return `{status, body}`.
- Persistent-connection channels (Discord Gateway, IRC, …) instead use
  `channel.conn.open({url})` in `onInit` and the callbacks `onConnOpen(id)`,
  `onConnMessage(id, text)`, `onConnClose(id, code)`, and `onTimer()` (a ~1s
  tick for heartbeats and reconnect timing). C owns the socket; the handler
  owns the protocol. See specs/channel-transports.md.

Host API: `channel.emit`, `channel.send`, `channel.http`,
`channel.conn.{open,send,close}` (persistent streams),
`channel.getConfig(key)` (registry, read-only),
`channel.getState/setState(key, value)` (runtime scratch),
`channel.log`, `channel.ackOutbox/failOutbox`,
and `channel.admin.isAdmin(id)` / `channel.admin.listPendingApprovals()` /
`channel.admin.dashboardUrl()`.

## Discord

Discord is a builtin channel (like telegram) reached over the real-time
**Gateway** (a persistent WebSocket): full DMs + guild messages, not just slash
commands. The handler owns the gateway protocol (IDENTIFY / RESUME / heartbeats
/ reconnect); C owns only the socket.

Turn it on:

1. Create an application + bot at the Discord Developer Portal; copy the bot
   token to `CCLAW_DISCORD_BOT_TOKEN` (or `save_secret discord.bot_token`).
2. **Enable the privileged `MESSAGE CONTENT` intent** on the bot's settings
   page. Without it the gateway closes with code 4014 and the handler logs a
   fatal error — messages would otherwise arrive with empty `content`. (SERVER
   MEMBERS / PRESENCE are not needed.)
3. Invite the bot to your server with the *Send Messages* permission (and
   *Read Message History* for reply context).
4. `set discord.admin_ids = <your user id>` and `discord.enabled = 1`, then
   promote/activate the channel.

Config keys beyond the standard `enabled`/`bot_token`/`admin_ids`/`base_url`:

- `require_mention` (default `1`) — in guild channels the bot stays silent
  unless it is @mentioned or its message is replied to. This is the cheap,
  deterministic gate: unmentioned chatter is dropped without spending a turn.
  DMs always reach the agent.
- `ambient_channels` — comma-separated channel ids (or `*`) where the bot
  instead sees **every** message. Pair with a route in `explicit` delivery
  mode for listen-and-decide: the agent reads the room and speaks only via
  `channel_send`. Costs a model turn per message — enable per channel.
- `allow_bots` (default `0`) — process messages from other bots. Off by
  default so several cclaw bots in one server can't loop on each other.
- `intents` — gateway intents bitfield (default `37377` = GUILDS |
  GUILD_MESSAGES | DIRECT_MESSAGES | MESSAGE_CONTENT).
- `egress_hosts` — the gateway (`gateway.discord.gg`), RESUME subdomains
  (`.discord.gg`), and the CDN (`.discordapp.net`) beyond `base_url`.

**Distinct personas = distinct bots.** A Discord application has one identity
(name + avatar) everywhere it appears, so give each persona its own bot token
and its own channel row — one `cclaw --channel` runner per persona — rather
than multiplexing one bot across channels.

## Message flow

```
Inbound:
  handler emit → channel_events → daemon routing gate → inbox → session entry
Outbound (auto mode):
  agent reply → channel_outbox → runner onOutbox → channel.send → platform
Outbound (explicit mode):
  agent calls channel_send tool → channel_outbox → runner onOutbox → platform
```
