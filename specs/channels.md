# Channels — runner model, lifecycle, routing, delivery

A channel connects CClaw to an outside surface (Telegram, a webhook, a custom
feed). This spec is the contract between the three parties involved:

> **Platform semantics in JS; authority in the DB, enforced in C; judgment in
> the agent.**

The channel handler (extension JS) reports platform *facts*. The daemon
enforces *authority* at routing time against `channel_chats` — fail-closed.
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

### Crash supervision

Two things matter when a runner dies, and they pull in opposite directions:
the channel is how the operator reaches the agent, so it must come back on its
own; and chat services meter reconnects, so it must not come back too often.

- **Never give up.** A dead runner is always respawned. There is no terminal
  state that waits for an operator who can no longer be reached.
- **Growing delay.** The respawn waits 2 s, 4 s, 8 s … up to five minutes
  (`CHANNEL_MAX_BACKOFF`), by the number of crashes in a row. The count resets
  once a runner has stayed up five minutes (`CHANNEL_HEALTHY_AFTER`). Either
  way a channel that cannot stay up reconnects at most about once per five
  minutes — under 300 session starts a day.
- **A bad swap is undone.** On the third crash in a row, a channel with a
  `prev_extension_name` (set by `channel swap`) is pointed back at it, the
  admins are told, and the count starts over. An extension that takes two
  minutes to die is reverted like one that dies at once. (The bounce that
  loads the new code is itself the first of the three.)
- **A swap that holds is accepted.** Once the new runner has stayed up five
  minutes, `prev_extension_name` is cleared. The predecessor is a revert
  target only while the new code is proving itself — a crash loop months
  later is an outage, not a bad swap, and must not roll the channel back.
  After that `channel revert` has nothing to revert to; swap by name instead.

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
indistinguishable from a crash to the rest of supervision — see Crash
supervision below). The mark is refreshed at every launch, respawn, and bounce, which is
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
`cclaw --doctor` prints the age of each channel's mark and the threshold in
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

**The raw envelope never reaches session entries.** The daemon
extracts plain text before the inbox insert: bare `text` for DMs,
`"<sender_name>: <text>"` for groups (attribution is load-bearing in
explicit-mode groups). A payload with no `$.text` (custom channel emitting its
own shape) passes through unchanged.

## Chats (`channel_chats`)

Every chat a channel has seen, or an operator or agent has named, has one
`channel_chats` row, keyed `(channel_name, chat_id)`; a chat cannot move
between channels. The row holds:

- **the pin** — `session_id`, the session the chat's messages go to (the
  session names its agent). NULL = not routed.
- **the chat's settings**, read live every turn: `delivery_mode` (how replies
  reach it — below), `tool_filter` (what its session may use — Authority
  attenuation) and `guidance` (prompt text — Guidance). Nothing is copied onto
  a session.
- **what traffic showed**: `title`, `kind` (`dm`/`group`, the envelope's
  `chat_type`), `is_admin` (a DM whose other party is an admin), `first_seen_at`,
  `last_message_at`, `error_notice_at`, `notified_at`.

A new row starts from defaults, **copied once**: `tool_filter` from
`channels.default_tool_filter` when the gate records the chat (none when an
operator or agent names it first); `delivery_mode` from what the envelope
says the chat is — a `group` starts `explicit`, a `dm` starts `auto`. A chat
named before it ever spoke (`cclaw route add`, an agent's `routes` request,
an operator naming it) has no envelope yet and is judged by its id: one that
begins with `-` (a Telegram group) starts `explicit`, any other `auto`; when
it then speaks, its first message does not overwrite the mode it was given.
After that only the row is consulted; changing the channel's default
changes future chats.

A row is never deleted: sessions and cron jobs point at their chat (declared
foreign keys), and older sessions left behind by `/new` keep naming it.

**`cclaw route rm` is the one way a routed chat becomes unrouted.** It is an
explicit operator act: the pin is cleared (`session_id` NULL), the row and
its settings stay, and `notified_at` is cleared so the operator hears once
more if the gate drops the chat's next message. Nothing else unroutes a chat
— moving its session elsewhere leaves it on a fresh session (below).

## Routing gate (C, fail-closed)

For each message event, in order (`channel_consume_events`, src/channel.c):

0. **Recorded** — the chat's row is created on first contact and its observed
   facts updated (`channel_chat_observe`), whatever happens next. An event
   for a channel that has no `channels` row is dropped.
1. **Routed** — the chat's row has a `session_id` → deliver to that session.
2. **Unrouted, open door** — `channels.default_agent` set → accept: create a
   session for that agent, pin the chat to it.
3. **Unrouted, admin** — the sender is in `<ext>.admin_ids` → accept via the
   global `default_agent` config (the operator always gets through), same
   create-and-pin.
4. **Unrouted, unknown** → drop + log + a **one-time admin notification**
   carrying sender id/name and the `cclaw route add` recipe. "Once" is
   recorded on the chat (`notified_at`), so a daemon restart does not repeat
   it; `cclaw route rm` clears it with the pin, so a chat the operator
   unrouted is reported the next time it speaks.

Chat membership is never authority. `admin_ids` is the only admin source.
`admin_chats` names the chats where operator messages are posted
([error-handling.md](error-handling.md#operator-notices)); being in one grants
nothing.

Admin chats can issue `/new` (re-point the pin at a fresh session for the
same agent) and `/sessions [id]` (list this chat's sessions / attach to
one), plus `/approve <id>` / `/deny <id>` (decide a parked approval) and
`/status` (what this chat is pinned to, and its reply setting) and
`/dashboard` (the tokenized link to the admin panel — the admin credential, so
it is sent only in a chat whose `channel_chats.kind` is `dm`; a group, or a
chat whose kind was never reported, gets a one-line refusal; if the web server
is not running or no token exists it says so). Handled in C
before any dispatch — authority actions that must work even when the pinned
session is wedged. A non-admin's `/new` is an ordinary message.

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

The pin IS the binding: a chat resolves to its `session_id`, and the session
names its agent (`sessions.agent_name`, immutable after creation). First
contact through the gate (open door / admin) creates the session and pins
the chat to it, so "every routed chat has exactly one current session" holds
from then on.

**One chat per session.** A session is pinned by at most one chat (a unique
index on `channel_chats(session_id)`), and a pinned session names the chat
that pins it (`sessions.channel_name/chat_id`, a declared link to the chat).
`channel_pin_session()` is the one function that moves a chat between
conversations — `/new`, `/sessions <id>`, `cclaw route add`, the dashboard's
attach, an agent's approved `routes` request and the gate all call it. Pinning
a session another chat holds **moves** it: that chat is left pinned to a fresh
session for the same agent (what `/new` would give it), in the same
transaction — a chat once routed is never silently unrouted. The session the
chat pinned before keeps naming the chat.

## Delivery modes

`channel_chats.delivery_mode` is the one value deciding how a session of
that chat delivers; it is read live at every turn boundary (the session's
`delivery_edges` row for the channel keeps only its cursor). Flipping it
between turns changes the very next turn. The full policy vocabulary
(`iteration`, `digest`, `turn`, `quiescent`, `explicit`) is in
[delivery.md](delivery.md). `cclaw route add --mode` accepts two values:

- **`auto`** (DM default, alias for `quiescent`): the final assistant message
  of a turn is inserted into `channel_outbox` for the origin chat.
- **`explicit`** (default for group chats): turn output stays in the
  session; the agent speaks only via `channel_send`. `channel_send` ships
  ungranted: an agent without the grant cannot speak in an `explicit` chat at
  all (its prompt says so) — grant it, or set the chat to `auto`. This is the group
  listen-and-decide pattern — `mentioned`/`reply_to_me` are the engage
  signals; silence is the default. A turn that ends in `explicit` is recorded
  as delivered-nothing, so a later flip to `auto` never posts it.

The agent is told which applies: the system prompt says how replies work in
this chat (Guidance, below).

## Where routes come from

Operator verbs (`cclaw route add`; on `/admin`, a chat's "Route to an agent",
an agent's "New session on a channel" / "Move a session to a chat", and a
channel's default agent) — or the agent itself: a `request_config` changes document
with a `routes` section (`["telegram:12345"]`,
approval-gated like every self-config write). Agent-requested routes are
**first-come** (a chat already routed to another agent is refused — an agent
must not capture another agent's chat), land with `delivery_mode='explicit'`
(the agent asked for send authority, not session mirroring) and no
`tool_filter` of their own (a chat the gate already recorded keeps the one it
started with). Note what the approver is granting: a route is send authority
*and* inbound routing — new senders in that chat will reach this agent.
Wildcard (`'*'`) routes are operator-only.

## Authority attenuation

Routes control *who gets in*; without attenuation a routed sender wields the
target agent's full authority. A chat may carry a `tool_filter` (JSON array
of tool names, `--tools` at `route add`; NULL = unrestricted). Every turn,
the tools of the session the chat pins are grants ∩ `sessions.tool_filter`
(a sub-agent's spawn scope) ∩ the chat's `tool_filter` **as it is now** —
enforced in payload assembly and again at dispatch. The filter can only
shrink authority, never widen it. An edit applies to the next request, and
`/new` cannot launder it away: the fresh session is narrowed by the same
chat. A session the chat no longer pins (left behind by `/new`) is not
narrowed by it.

Chats the gate records start from the channel's own filter,
`channels.default_tool_filter` (`route add <channel> '*' <agent> --tools
...`; NULL = unrestricted). This is the attenuated tool set for a bot that is
reachable server-wide — a Discord @mention from any public channel — while an
explicitly routed chat keeps its own filter, or full authority if it has
none.

Admins are **not** exempt: an admin reaching the default agent through an
unrouted chat is still an unrouted chat, and gets the same default filter.
Admin authority comes from the explicitly routed admin channel, never
ambiently from being in `admin_ids`.

## Guidance (the chat's system prompt text)

A session that belongs to a chat gets three parts appended to its system
prompt, in order, read every request: where it is chatting (the chat's
`title`, else its ids), **how replies work there** (built in, from
`delivery_mode`: "Your final message of the turn is posted to this chat." /
"Nothing you write is posted to this chat. To say something here, call
channel_send." — or, for an agent whose tools this turn do not include
`channel_send`, "Nothing you write is posted to this chat, and you have no
channel_send tool, so you cannot reply here."), then the chat's `guidance` (`cclaw route add ... --prompt
"<text>"`) — room etiquette, house style, "you are in the #ops channel". It
is *prompt text, not authority*: it can only change how the agent speaks,
never what it may do (that stays in `grants` + `tool_filter`). The text
changes only when the chat's row does, so the request prefix stays
byte-stable for prompt caching otherwise. A session with no chat gets
nothing appended.

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
session pinned by a chat on an **ambient channel resolves that window to 0** —
the notice is written inline at park time and the turn continues immediately,
because a silent room reads as a hung bot rather than as work in progress. The
approval card still posts at t=0 either way; only the freeze differs.
`<channel>.ambient_channels` is the same key the runner reads, so the
convention carries to any channel extension.

## channel_send (outbound tool)

Fixed schema `{channel, chat_id, message, file}` (+ `action: "list"` to
enumerate reachable targets). **Routed chats are the allowlist,
default-deny**: a target needs an exact `(channel, chat_id)` chat whose
pinned session belongs to the sending agent (`send_target_allowed`,
`src/tool_channel_send.c`). `channels.default_agent` grants no send
authority; reaching a new target is an operator act (`cclaw route add`). The
tool inserts an outbox row and returns `queued, outbox id N` — delivery is
asynchronous with the existing pending→sending→delivered/failed progression.
Text to the session's own chat while that chat posts replies by itself (any
`delivery_mode` but `explicit`) short-circuits — the reply already delivers
there. It reads the same live value the turn boundary delivers by, so the two
can never disagree (review-7: they once read a route template and a frozen
copy, and a reply could be refused here and never delivered).
Ships **ungranted**; the operator grants it per agent.

### Files

`file` names a file in the sending agent's workspace; `message` is then its
caption and may be omitted. This is how an agent shows a person a picture.

- **The file is judged, then copied.** The tool runs in the trusted parent
  and the path comes from the model, so it is opened with
  `util_open_inside`: the check is made on the open descriptor
  (`/proc/self/fd`), not on the path, so a symlink anywhere on the way is
  resolved before it is judged and nothing can be swapped between the check
  and the read. The bytes are copied to the channel's media spool
  (`<db_dir>/media/<channel>/out-…`), which the agent cannot write. Size is
  capped by `channel_send_file_max_bytes` (default 10 MB).
- **The outbox row** is `{chat_id, caption, file: {path, name, mime}}` — no
  `text`. A handler written before files existed therefore fails the row
  ("missing chat_id or text") and the agent is told, instead of delivering
  the caption alone. `mime` comes from the file's first bytes.
- **The handler uploads it** with `channel.send({..., form, file})`
  (multipart/form-data, see [extensions.md](extensions.md)). The runner
  reads the file from disk as it sends and accepts only a path inside its
  own channel's spool. Discord posts it as an attachment; Telegram sends an
  image as a photo and falls back to a document on the plain re-delivery;
  WhatsApp fails the row, because its bridge takes text only.
- **The copy lives as long as the row can still be sent**:
  `channel_ack_outbox` and `channel_fail_outbox` remove it.
- **The auto-mode short-circuit does not apply**: a turn's reply never
  carries a file, so sending one to the session's own chat is not a
  double post.

For a file a chat cannot carry there is `share_file` (see
[operations.md](operations.md#shared-files--share)).

## Ingestion-only channels

A handler that defines no `onOutbox` is a pure source. The runner records
`channel_state.has_outbox='0'` at load time (startup and `--check`), and the
daemon skips the per-turn outbox insert for its sessions — otherwise rows
would sit pending forever. Absent key = assume duplex.

## Operator surface

```
cclaw route add <channel> <chat_id> <agent> [--mode auto|explicit] [--session <id>] [--tools name,name,...] [--prompt <text>]
cclaw route rm  <channel> <chat_id>
cclaw route list
```

`route add` pins the chat (a fresh session, or `--session`'s, which must
belong to `<agent>`) and sets the chat's mode and tools; `--prompt` sets its
guidance (omitted: kept; `""`: cleared). Without `--mode`, a chat that has
spoken takes the mode its kind implies (group `explicit`, DM `auto`), one
that never spoke is judged by its id (negative: `explicit`). `route rm` unpins the chat and keeps its row.
`route list` shows every chat, routed first, then those nobody routed (with
when they last spoke and whether the operator was told). Chat id `'*'` sets
or clears `channels.default_agent` (and, with `--tools`,
`channels.default_tool_filter`) instead of adding a route.
