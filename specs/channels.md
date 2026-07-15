# Channels — runner model, lifecycle, routing, delivery

A channel connects CClaw to an outside surface (Telegram, a webhook, a custom
feed). This spec is the contract between the three parties involved:

> **Platform semantics in JS; authority in the DB, enforced in C; judgment in
> the agent.**

The channel handler (extension JS) reports platform *facts*. The daemon
enforces *authority* at routing time against `channel_routes` — fail-closed.
The agent decides *when to speak* in listen-and-decide chats.

## Process model

- The daemon fork+execs `cclaw --channel <name>` per active channel (one
  binary, every mode — see AGENTS.md). The runner loads the handler file
  resolved through `channels.extension_name → extensions.path`, runs the QJS
  event loop, and talks to the daemon only through the DB plus wake pipes.
- Inbound: handler calls `channel.emit("message", envelope_json,
  external_id)` → `channel_events` row → daemon's `channel_consume_events()`
  routes it (below) → `inbox` → session entry.
- Outbound: `channel_outbox` rows (pending → sending → delivered/failed,
  crash-recovered at runner startup) drained by the runner's `onOutbox`.
- Lifecycle: rows land in `channels.status='draft'` at install/promote;
  `--check` validates (manifest + JS load + onInit), activation is an operator
  or trust-flow act; `channel swap`/`revert` change which extension fronts a
  name. The launch gate additionally requires `<ext>.enabled` truthy and all
  `required` config keys resolvable (specs/config.md).

## Envelope schema (JS → C)

`channel.emit` message payloads are JSON with these fields (additive-only for
compatibility; all but `channel_id` optional):

| Field | Meaning |
|-------|---------|
| `channel_id` | Chat/conversation id — the routing key |
| `text` | The message text (or media caption) |
| `sender_id` | Platform sender id (in a DM usually == channel_id; in a group the person, not the group) |
| `sender_name` | Display name, attribution only |
| `chat_type` | `dm` or `group` |
| `mentioned` | bool — the bot was @-mentioned (telegram: needs getMe identity; false until learned) |
| `reply_to_me` | bool — this message replies to one of ours |
| `media` | `{kind, mime, path}` — diverts to media preprocessing before routing |

Facts only: the handler never makes authority decisions — extension JS is not
a trust boundary.

**The raw envelope never reaches session entries** (review-1 F19). The daemon
extracts plain text before the inbox insert: bare `text` for DMs,
`"<sender_name>: <text>"` for groups (attribution is load-bearing in
explicit-mode groups). A payload with no `$.text` (custom channel emitting its
own shape) passes through unchanged.

## Routing gate (C, fail-closed)

For each message event, in order (`channel_consume_events`, src/channel.c):

1. **Routed** — `channel_routes` hit, exact `(channel, channel_id)` first,
   then the `(channel, '*')` wildcard → deliver.
2. **Unrouted, admin** — `channel_id ∈ <ext>.admin_ids` → accept via
   `default_agent`, auto-creating a session (the operator always gets
   through).
3. **Unrouted, `<ext>.allow_unknown=1`** → accept via `default_agent` (the
   pre-gate open behavior, off by default).
4. **Unrouted, unknown** → drop + log + a **one-time admin notification**
   carrying sender id/name and the `cclaw route add` recipe. The dedup set is
   in-memory per process — ephemeral politeness state, not authority; a
   re-notify after daemon restart is acceptable.

Chat membership is never authority. `admin_ids` is the only admin source.

## Session resolution

A route with non-NULL `session_id` **pins** the chat to that session (exact
route beats wildcard; a pin to a deleted session falls back). Otherwise:
find-latest session for `(channel_name, channel_id)`, create one under the
route's agent if none. Route→agent is only the "which agent gets a *new*
session" rule; the session names its agent thereafter.

## Delivery modes

`channel_routes.delivery_mode`:

- **`auto`** (DM default): assistant turn output is inserted into
  `channel_outbox` for the origin chat, as always.
- **`explicit`** (default for group-shaped ids at `route add`): turn output
  stays in the session; the agent speaks only via `channel_send`. This is the
  group listen-and-decide pattern — `mentioned`/`reply_to_me` are the engage
  signals; silence is the default.

No route = `auto` (covers admin/allow_unknown flows).

## Where routes come from

Operator verbs (`cclaw route add`, dashboard `set_channel_route` /
`attach_channel`) — or the agent itself: `request_config` action
`request_changes` with a `routes` section (`["telegram:12345"]`,
approval-gated like every self-config write). Agent-requested routes are
**first-come** (a chat already routed to another agent is refused — an agent
must not capture another agent's chat), land with `delivery_mode='explicit'`
(the agent asked for send authority, not session mirroring) and no
`tool_filter`. Note what the approver is granting: a route is send authority
*and* inbound routing — new senders in that chat will reach this agent.
Wildcard (`'*'`) routes are operator-only.

## Authority attenuation

Routes control *who gets in*; without attenuation a routed sender wields the
target agent's full authority. A route may carry a `tool_filter` (JSON array
of tool names, `--tools` at `route add`; NULL = unrestricted). When a channel
event **creates** a session for that route, the filter is copied onto
`sessions.tool_filter` — the same frozen-at-spawn semantics as sub-agent
filters. Per turn the effective tool set is grants ∩ filter, enforced in
payload assembly and dispatch; the filter can only shrink authority, never
widen it.

Frozen means frozen: editing the route's filter later does not retro-apply to
existing sessions — a new session must be created for the change to take
effect. Exact route beats wildcard when resolving the filter (an exact route
with no filter deliberately overrides a filtered `'*'` route). Unrouted
admin-accepted senders get no filter (admin = operator; no attenuation).

## channel_send (outbound tool)

Fixed schema `{channel, chat_id, message}` (+ `action: "list"` to enumerate
reachable targets); text-only v1. **Routes are the allowlist, default-deny**:
a target needs an exact `(channel, chat_id)` route resolving to the sending
agent — by `agent_name` or through a pinned session the agent owns. A `'*'`
route grants no send authority; reaching a new target is an operator act
(`cclaw route add`). The tool inserts an outbox row and returns
`queued, outbox id N` — delivery is asynchronous with the existing
pending→sending→delivered/failed progression. Sends to the session's own
origin chat in `auto` mode short-circuit (the reply already delivers there).
Ships **ungranted**; the operator grants it per agent.

## Ingestion-only channels

A handler that defines no `onOutbox` is a pure source. The runner records
`channel_state.has_outbox='0'` at load time (startup and `--check`), and the
daemon skips the per-turn outbox insert for its sessions — otherwise rows
would sit pending forever. Absent key = assume duplex.

## Operator surface

```
cclaw route add <channel> <chat_id> <agent> [--mode auto|explicit] [--session <id>] [--tools name,name,...]
cclaw route rm  <channel> <chat_id>
cclaw route list
```

Group-shaped (negative) chat ids default to `--mode explicit`.
