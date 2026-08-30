# WhatsApp channel — bridge wire API + channel extension

WhatsApp has no bot API: speaking it means implementing the multi-device Noise
protocol, Signal sessions, and media crypto. That work does not belong in the
cclaw binary or in channel JS. It lives in a **bridge**: a separate process
that speaks WhatsApp on one side and a deliberately boring HTTP/JSON API on
the other. The cclaw side is an ordinary poll-transport channel extension
(`templates/channel_whatsapp.qjs`) that talks to the bridge exactly the way
the Telegram channel talks to `api.telegram.org` — the runner cares only that
it is a URL.

> **The API in this document is the contract; the bridge is replaceable.**
> The reference bridge is `bridge/whatsapp/` (Rust, on
> [whatsapp-rust](https://github.com/oxidezap/whatsapp-rust), builds down to
> ARMv5TE). But anything that implements this API — a Baileys wrapper in Node
> on another machine, whatsmeow in Go — works unchanged: point the channel's
> `base_url` at it. That is why the API is versioned, minimal, and defined
> here rather than by whatever the Rust code happens to expose.

## Topology

```
WhatsApp servers ⇄ [bridge process]  ⇄ HTTP :8471 ⇄  cclaw --channel whatsapp
                    owns wa creds,                     ordinary poll channel,
                    session store,                     emits envelopes,
                    protocol state                     drains outbox
```

- The bridge is its own supervised service (init script / systemd unit),
  **not** a child of cclaw. The channel treats "connection refused" as an
  ordinary failed poll — the runner's existing backoff applies, and the
  channel recovers by itself when the bridge comes back. Neither process
  manages the other's lifecycle.
- Default deployment is same-host loopback. Cross-host is legal (set
  `base_url`, set an API token, arrange transport security yourself — e.g. a
  WireGuard link); the egress pin derives from `base_url` as usual.

## Bridge HTTP API (v1)

All endpoints are JSON over HTTP under `/v1/`. Errors are
`{"error": "<message>"}` with a 4xx/5xx status.

**Auth.** If the bridge is configured with a token, every request must carry
it — either `Authorization: Bearer <token>` or a `?token=<token>` query
parameter. (The query form exists because the runner's recurring poll shape
carries no headers; loopback deployments may run tokenless.) Wrong/missing
token → 401.

### GET /v1/status

```json
{ "state": "starting" | "pairing" | "connected" | "disconnected" | "logged_out",
  "jid": "15551234567@s.whatsapp.net",   // present once known
  "pair_code": "ABCD-EFGH",              // present while pairing via pair-code
  "bridge": "whatsapp-bridge", "version": "0.1.0", "api": 1 }
```

### GET /v1/events?cursor=N&timeout=S

Long-poll. Blocks up to `timeout` seconds (cap 30; default 25) until at least
one event with id > `cursor` exists, then returns everything after `cursor`:

```json
{ "cursor": 42, "events": [ ... ] }
```

The caller persists `cursor` and passes it back. Cursor semantics:

- Ids are per-bridge-process, monotonically increasing. **The buffer is
  memory, not a journal**: a bridge restart resets ids and drops undelivered
  events. A `cursor` the bridge does not recognize (ahead of its tail, i.e.
  from a previous incarnation) is answered immediately with the *current*
  cursor and no events — the client resynchronizes, it never errors.
- Delivery is therefore at-least-once within a bridge lifetime and
  best-effort across restarts. The channel emits every message with
  `external_id = "wa_" + msg_id`, so cclaw's emit dedup absorbs replays.

Event kinds (additive; consumers ignore unknown `type`):

```json
{ "id": 7, "type": "message",
  "msg_id": "3EB0...",                 // WhatsApp message id (dedup key)
  "chat_id": "1555...@s.whatsapp.net", // or ...@g.us for groups
  "sender_id": "1555...@s.whatsapp.net",
  "sender_name": "Ana",                // push name; attribution only
  "chat_type": "dm" | "group",
  "text": "hello",                     // text or media caption; text-only v1
  "mentioned": false,                  // our jid in the mention list
  "reply_to_me": false,                // quoted message is ours
  "ts": 1756500000 }

{ "id": 8, "type": "status", "state": "connected" }      // state transitions
{ "id": 9, "type": "status", "state": "pairing", "pair_code": "ABCD-EFGH" }
```

### POST /v1/send

```json
→ { "chat_id": "1555...@s.whatsapp.net", "text": "reply text" }
← { "ok": true, "message_id": "3EB0..." }
```

Text-only in v1. 4xx on a malformed JID, 502 with `{"error":...}` if the
send fails upstream (not connected, etc.) — the outbox ladder in C retries
5xx and fails terminally on 4xx, which is exactly the split we want.

### POST /v1/pair *(optional)*

```json
→ { "phone": "15551234567" }        // digits only, country code, no '+'
← { "ok": true }
```

Starts (or restarts) the pair-code flow for that number; the code itself
arrives as a `status` event and in `/v1/status.pair_code`. A bridge MAY
implement this; one that does not answers 501. The reference bridge does
not in v1 — pairing is an operator act (once per account), configured
bridge-side (`WAB_PHONE`) and done at boot; QR pairing needs no endpoint
either (an unpaired bridge emits the QR to its own log and as `status`
events).

## Reference bridge (`bridge/whatsapp/`)

Config is environment only, mirroring cclaw's own ethos:

| Env | Default | Meaning |
|-----|---------|---------|
| `WAB_LISTEN` | `127.0.0.1:8471` | Bind address. Never bind non-loopback without a token. |
| `WAB_TOKEN` | *(empty)* | API token; empty = no auth (loopback only). |
| `WAB_DB` | `whatsapp.db` | WhatsApp session store (SQLite). **Credentials — protect like `.cclaw_key`.** |
| `WAB_PHONE` | *(empty)* | If set and unpaired, start pair-code flow for this number at boot. |

The bridge never exits on a session ending (QR codes expired unscanned,
logged out, transport lost): it reports `disconnected`, waits 60 s, and
starts a fresh session on the same store — so an unpaired bridge re-offers
pairing indefinitely and a paired one reconnects. Supervision only has to
handle a crash.

Event buffer: last 512 events in memory. WhatsApp itself queues offline
messages server-side and replays them on reconnect, so bridge downtime loses
nothing end-to-end; only events received while no client polls *and* the
buffer wraps are dropped (a poller 512 messages behind has bigger problems).

Built by CI for each release target the same way cclaw itself is; the
armv5te build is the reason the bridge exists (Pogoplug-class boxes). The
binary is attached to cclaw releases as `whatsapp-bridge-linux-<arch>`.

## The channel extension

`templates/channel_whatsapp.qjs`, builtin bundle `whatsapp` — poll transport
only, no persistent conn, no webhook. Config keys (see manifest):

- `enabled` (0/1), `base_url` (default `http://127.0.0.1:8471`),
  `api_token` (secret, optional), `admin_ids` (comma-separated JIDs).
- Poll loop: `GET /v1/events` with the persisted cursor
  (`channel_state` key `wa_cursor`); 25 s long-poll under the runner's 35 s
  poll timeout.
- `message` events → the standard envelope (`chat_id`, `text`, `sender_id`,
  `sender_name`, `chat_type`, `mentioned`, `reply_to_me`), external id
  `wa_<msg_id>`. Routing, admin authority, delivery modes are the generic
  machinery in [channels.md](channels.md) — nothing WhatsApp-specific.
- `status` events → `channel.log` (this is where the operator reads the pair
  code during setup: `journalctl`/daemon log, or `/v1/status` directly).
- Outbox → `POST /v1/send` via `channel.send` (C owns ack/retry). Messages
  are sent whole — WhatsApp's text limit (~64 KB) exceeds any sane turn
  output; no chunking in v1.

## Account requirements (operational, not code)

- **Use a dedicated number.** Unofficial clients violate WhatsApp ToS and
  bans happen; never link a number you care about. A prepaid/burner SIM
  registered on a real phone is the standard setup.
- The registering phone anchors the account: it must come online roughly
  every 2 weeks or linked devices (the bridge) are logged out. `logged_out`
  state means re-pairing.

## Non-goals (v1)

- Media in either direction (bridge drops non-text; captions pass as text).
- Groups metadata (subject/participant names beyond push names), reactions,
  edits, receipts.
- Multi-account. One bridge = one WhatsApp account = one channel.
