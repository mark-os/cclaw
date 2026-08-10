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
- Transports (how bytes reach the handler — poll, webhook, persistent stream):
  [channel-transports.md](channel-transports.md).
- Lifecycle: rows land in `channels.status='draft'` at install/promote;
  `--check` validates (manifest + JS load + onInit), activation is an operator
  or trust-flow act; `channel swap`/`revert` change which extension fronts a
  name. The launch gate additionally requires `<ext>.enabled` truthy and all
  `required` config keys resolvable (specs/config.md).

### Deaf-channel watchdog

Supervision only ever hears about a runner that *exits*, but a runner can be
alive and have silently stopped receiving — a wedged WebSocket, a long-poll
loop that never completes again. The runner therefore stamps
`channel_state.last_activity_at` on **received transport traffic**, not on
user-visible messages: any inbound WebSocket frame (gateway heartbeat ACKs
included — Discord's arrive every ~41s) and every successful poll cycle,
throttled to one write per 5s. That distinction is the whole design — an idle
chat must not look like a broken one, and a wedged transport stops ACKing
within a minute or two, which is exactly the signal.

`channel_tick` bounces a tracked channel whose mark is older than its
threshold: `SIGTERM` plus a `channel deaf …` error log, then the ordinary
reap → backoff → respawn path does the restart (so a bounce is
indistinguishable from a crash to the rest of supervision, flap detection
included). The mark is refreshed at every launch, respawn, and bounce, which is
both the grace period — a fresh runner gets a full threshold to connect,
whatever staleness it inherited — and the debounce for a child that ignores
`SIGTERM`.

Enrolment is by first traffic: only the runner's *received-traffic* stamp
creates the row; every other write refreshes an existing one. A channel with no
self-driven transport (a webhook, fed by the daemon over the request UDS) never
stamps, has no row, and is never judged deaf — there is nothing there that
could wedge.

Threshold resolution: `<ext>.deaf_timeout`, then the global
`channel_deaf_timeout` (env `CCLAW_CHANNEL_DEAF_TIMEOUT`), then the built-in
`CHANNEL_DEAF_TIMEOUT` (300s). Either key set to `0` turns the watchdog off.
`cclaw doctor` prints the age of each channel's mark and the threshold in
force.

## Envelope schema (JS → C)

`channel.emit` message payloads are JSON with these fields (additive-only for
compatibility; all but `chat_id` optional):

| Field | Meaning |
|-------|---------|
| `chat_id` | Chat/conversation id — the routing key |
| `text` | The message text (or media caption) |
| `sender_id` | Platform sender id (in a DM usually == chat_id; in a group the person, not the group) |
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

1. **Routed** — exact `(channel, chat_id)` hit in `channel_routes` → deliver
   to the pinned session (the session names its agent).
2. **Unrouted, open door** — `channels.default_agent` set → accept: create a
   session for that agent, pin the chat to it.
3. **Unrouted, admin** — `chat_id ∈ <ext>.admin_ids` → accept via the global
   `default_agent` config (the operator always gets through), same
   create-and-pin.
4. **Unrouted, unknown** → drop + log + a **one-time admin notification**
   carrying sender id/name and the `cclaw route add` recipe. The dedup set is
   in-memory per process — ephemeral politeness state, not authority; a
   re-notify after daemon restart is acceptable.

Chat membership is never authority. `admin_ids` is the only admin source.

Admin chats can issue `/new` (re-point the pin at a fresh session for the
same agent) and `/sessions [id]` (list this chat's sessions / attach to
one), plus `/approve <id>` / `/deny <id>` (decide a parked approval) and
`/status` (what this chat is pinned to). Handled in C before any dispatch —
authority actions that must work even when the pinned session is wedged. A
non-admin's `/new` is an ordinary message.

Administration is three separated concerns (`src/channel_intent.c`):
**surface interpretation** turns platform input into a structural
`ChannelIntent` (the daemon parses chat text; the extension interprets its
own surfaces — an inline-button tap arrives as an `approval_decision` event
and becomes the same DECIDE intent); **authority**
(`channel_intent_allowed`) is the single policy point — text commands
require an `admin_ids` sender, structural events are trusted because the
emitting extension gates the sender itself; **execution**
(`channel_intent_execute`) checks object scope/state and performs the read
or transition. The consumer wires parse → allow → execute; anything else
falls through as conversation.

`/approve` and `/deny` are a scope + state check in front of
`resolve_approval()` — the same entry point the dashboard, the CLI prompt and
the inline-button `approval_decision` event use. An approval is decidable only
from the channel its session is bound to (a mismatch reads as "no such
approval"), only while `state='pending'`, and the decision is attributed in
`approvals.decided_via` as `channel:<channel>:<sender_id>`. Strength is
deliberately minimal: a `rerun` approval resolves ONCE, an `apply` grant
ALWAYS (once is incoherent for it) — "allow and stop asking" stays a
dashboard/button decision.

## Session resolution

The pin IS the binding: a route resolves to its `session_id`, and the
session names its agent (`sessions.agent_name`, immutable after creation).
First contact through the gate (open door / admin) creates the session and
writes the pin back, so the invariant "every routed chat has exactly one
current session" holds from then on. Re-pointing the pin (`/new`,
`/sessions <id>`, `cclaw route add`) is the one way to move a chat between
conversations; the FK refuses to delete a session a route still pins.

## Delivery modes

`channel_routes.delivery_mode`:

- **`auto`** (DM default): assistant turn output is inserted into
  `channel_outbox` for the origin chat, as always.
- **`explicit`** (default for group-shaped ids at `route add`): turn output
  stays in the session; the agent speaks only via `channel_send`. This is the
  group listen-and-decide pattern — `mentioned`/`reply_to_me` are the engage
  signals; silence is the default.

Gate-created pins start `auto` (the column default).

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
effect.

Chats the gate accepts **without** a route take the channel's own filter,
`channels.default_tool_filter` (`route add <channel> '*' <agent> --tools
...`; NULL = unrestricted, the pre-existing behaviour). This is the
attenuated tool set for a bot that is reachable server-wide — a Discord
@mention from any public channel — while an explicitly routed chat keeps its
own filter, or full authority if it has none.

Admins are **not** exempt: an admin reaching the default agent through an
unrouted chat is still an unrouted chat, and gets the same default filter.
Admin authority comes from the explicitly routed admin channel, never
ambiently from being in `admin_ids`. `/new` in an unrouted chat re-freezes
the same default filter, so it can't launder the open door into full tools.

## Per-route system prompt suffix

`channel_routes.system_prompt_suffix` (`cclaw route add ... --prompt "<text>"`)
is appended to the system prompt of the route's pinned session on every turn —
room etiquette, house style, "you are in the #ops channel". It is *prompt
text, not authority*: it can only change how the agent speaks, never what it
may do (that stays in `grants` + `tool_filter`), which is why it is read live
rather than frozen at session creation like the filter. Nothing is appended
when it is NULL, and the request body is byte-identical to today's.

## Ambient debounce (Discord)

An ambient channel hands the agent every message in the room, so a burst of
chatter would spend one turn per line. `debounce_ms` (0 = off) batches a burst
into ONE envelope: the first message into a quiet chat still emits
immediately (leading edge), the rest buffer and flush `debounce_ms` after the
*first* buffered message — a latency cap, so a room that never falls silent
still gets answered. An @mention or a reply flushes the whole buffer at once,
with the mentioning message included. A batched envelope carries pre-joined
`Name: text` lines and an empty `sender_name`, so the group formatter passes
the already-attributed text through unchanged. DMs and mention-gated channels
are never debounced. Buffers are runner-process memory: a crash loses at most
one window of chatter nobody addressed to us.

Ambience also decides how an approval behaves. A parked approval normally
freezes the turn for `approval_block_sec` (60s) so a present approver's tap
resumes the frozen call in place; past that the sweep unparks it with a
background notice and the late decision arrives as an inbox follow-up. A
session pinned to a route on an **ambient channel resolves that window to 0** —
the notice is written inline at park time and the turn continues immediately,
because a silent room reads as a hung bot rather than as work in progress. The
approval card still posts at t=0 either way; only the freeze differs.
`<channel>.ambient_channels` is the same key the runner reads, so the
convention carries to any channel extension.

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
