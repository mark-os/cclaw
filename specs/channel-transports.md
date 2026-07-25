# Channel transports — the persistent-connection primitive

Companion to [channels.md](channels.md). That spec covers routing, delivery, and
the JS↔C envelope; this one covers **how bytes reach the handler**.

> **C owns the transport; JS owns the protocol.** The runner exposes a small,
> platform-agnostic set of transport primitives. Everything above the byte layer
> — opcodes, heartbeats, reconnect logic, message framing semantics — lives in
> the `.qjs` handler. C never learns a channel is "Discord."

## The four transport shapes

A channel needs bytes to move in one of four shapes. Three predate this spec:

| Shape | Owner | Mechanism | JS surface |
|-------|-------|-----------|------------|
| One-shot outbound req/resp | runner (curl) | host-pinned HTTP | `channel.http` / `channel.send` |
| Inbound **pull** | runner (curl) | recurring long-poll | `onInit`/`onPoll` → `{poll: Req}` |
| Inbound **push (HTTP)** | daemon (civetweb) | `/hook/<channel>` → UDS proxy | `onRequest(req)` |
| **Persistent bidirectional stream** | runner (curl) | this spec | `channel.conn.*` + `onConn*` |

The runner never binds a network port; inbound push funnels through the daemon's
single civetweb port. The persistent-stream shape is the runner's *own* socket,
polled inside its existing event loop — no new listener, no new attack surface.

## `channel.conn.*` — persistent connections

One primitive covers every persistent transport. **Framing** selects how bytes
are chunked into messages; the protocol on top is always the handler's job.

| Framing | Transport | libcurl | Covers |
|---------|-----------|---------|--------|
| `ws` (default) | WebSocket (WS/WSS) | `CONNECT_ONLY=2` + `curl_ws_recv/send` | Discord Gateway, Slack Socket Mode, realtime voice |
| `raw` | TCP, optionally TLS | `CONNECT_ONLY=1` + `curl_easy_recv/send` | IRC, XMPP, MQTT, line/length-framed protocols |

`raw` is a **planned second framing**, not part of the first cut — but the C
structure is built around framing from day one so `raw` is a branch, not a
rewrite. Ship `ws`; add `raw` when a channel needs it.

### Host functions (JS → C)

```
channel.conn.open(spec) -> id            // int id >= 1, or throws
    spec = { url, framing?: "ws"|"raw", headers?: ["Name: v"], timeout? }
    framing defaults from the URL scheme (wss/ws -> "ws").
    The handshake is synchronous (blocking TCP+TLS+upgrade); open() returns
    once connected. A small fixed cap on live conns per channel (CR_CONN_MAX).

channel.conn.send(id, text) -> bool      // text frame (ws) / bytes (raw)
    Queues on CURLE_AGAIN, flushed on the next tick. false if id unknown/closed.
    Text-only in v1 (no binary ws frames).

channel.conn.close(id)                   // graceful close; fires onConnClose
```

### Callbacks (C → JS, all optional)

```
onConnOpen(id)              // handshake complete, before any message
onConnMessage(id, text)     // one complete message: a reassembled ws text
                            //   frame, or a raw byte chunk (raw framing =
                            //   handler reassembles; C guarantees nothing
                            //   about chunk boundaries)
onConnClose(id, code)       // peer/error/local close; handler reconnects
                            //   (with its own backoff) by calling open() again
onTimer()                   // ~1s tick (see below) — heartbeats, zombie checks
```

The reconnect state machine (Discord RESUME: save `session_id` +
`resume_gateway_url`, replay on reconnect) is **entirely JS** and testable
against a loopback mock without touching the real platform.

## The timer tick

`onTimer()` fires once per event-loop iteration. The `curl_multi_poll` timeout
is 1000 ms, so that is the *slowest* it ticks, not the rate: poll also returns
as soon as any fd is readable, so a busy channel ticks considerably more often
than 1 Hz. Treat it as "at least once a second", never as a clock — which is
why the handler tracks its own deadline against wall-clock. It is the generic
periodic primitive: the handler tracks its own deadline against wall-clock (JS
`Date.now()`) and acts when due. Chosen over a `setTimer(ms)` scheduler because
one unconditional tick is simpler, and handlers already need wall-clock deadline
tracking for zombie-connection detection. Sub-second precision is out of scope;
no channel protocol needs it (Discord's heartbeat interval is ~41 s).

## Loop integration (C responsibilities)

- Each open conn contributes its `CURLINFO_ACTIVESOCKET` fd to the runner's
  `extra[]` pollfd array (today: outbox FIFO + request UDS; grows by
  `CR_CONN_MAX`).
- Drain each conn **unconditionally every iteration**, not just when the fd
  polls readable: a `CONNECT_ONLY` handle can hold frames already read off the
  socket into curl's own buffer (e.g. a Discord `HELLO` that arrived *during*
  the handshake). Those bytes are off the OS socket, so `poll()` reports the fd
  idle and gating on `revents` would strand them until the next inbound byte.
  `curl_ws_recv` returns `CURLE_AGAIN` cheaply when empty, so an idle drain is
  nearly free. Drain until `CURLE_AGAIN`, **reassembling a frame split across
  reads** (`curl_ws_frame.bytesleft`) into one message before dispatch.
  - *Consequence:* when the only pending data is a frame already buffered inside
    curl, `curl_multi_poll` has nothing at the OS level to wake on and may sleep
    its full 1000 ms before the unconditional drain picks it up — a one-time
    sub-second latency on such a message. Harmless at Discord's timescales
    (heartbeat ~41 s, IDENTIFY window generous); not worth a self-pipe wakeup.
- **C consumes ws control frames** — replies to `PING` with `PONG`, turns a
  `CLOSE` frame into `onConnClose(id, code)`. Only `TEXT` (reassembled) reaches
  `onConnMessage`. (Discord liveness is app-level op 1/11, not ws ping/pong;
  control-frame handling is pure transport hygiene the handler never sees.)
- Call `onTimer()` every iteration; free all conns on the cleanup path.

## Egress pinning

Persistent-conn URLs are checked against the channel's egress allowlist, exactly
like `channel.http` (`url_host_allowed`, default-deny). A channel that legitimately
talks to several hosts (Discord: `discord.com` for REST **and**
`gateway.discord.gg` for the Gateway) needs more than the single `base_url` pin:
the allowlist is `base_url`'s host plus any entry in an optional comma-separated
`egress_hosts` config key.

### Exact vs. suffix matching

`url_host_allowed` matches **exact host by default**; a leading-dot entry
(`.discord.gg`) is an opt-in **suffix** match. Suffix semantics, mirroring a
proven design (OpenClaw's `isHostnameAllowedByPattern`):

- `.discord.gg` matches `gateway-us-east1-d.discord.gg` (dot-boundary required),
- also matches the bare apex `discord.gg` — a suffix entry covers the registered
  domain *and* its subdomains, so `.discord.gg` need not be paired with a second
  exact entry,
- never matches `evildiscord.gg`: the match must land on a dot boundary, so a
  longer label ending in the same characters is refused.

The apex is deliberate and shared with `grants` (`host_match` is one matcher for
both). It means a suffix entry is *wider* than an exact one in both directions —
which is why the trust rule below is a rule and not a suggestion.

Suffix matching exists because the exact-host stance breaks on two runtime-dynamic
patterns, and **media/CDN downloads — not the Gateway — are the primary driver**:
a Discord attachment lives on `cdn.discordapp.com`/`*.discordapp.net`, Slack files
rotate across `*.slack-files.com`/`*.slack-edge.com`. Discord's RESUME subdomain
(`resume_gateway_url` under `.discord.gg`) is the same shape, and once suffix
matching exists for media it covers RESUME for free — so the handler dials the
issued `resume_gateway_url` (honoring Discord's routing) rather than falling back
to the fixed gateway.

> **Trust rule — provider-exclusive domains only.** A suffix entry is safe only
> when the provider controls *every* subdomain (`.discord.gg`, `.discordapp.net`,
> `.slack-edge.com`). **Never** wildcard shared cloud storage where a third party
> can register a subdomain (`.s3.amazonaws.com`, `.myqcloud.com`) — that reopens
> the exfiltration path the pin exists to close. C cannot tell the two apart;
> this is an extension-author / operator responsibility, enforced by review of
> the `egress_hosts` config, not by code.

> **The bare `*` entry disables the pin entirely.** `url_host_allowed` shares
> `host_match`'s rule vocabulary, so a lone `*` in `egress_hosts` is the
> allow-all sentinel — it matches every host, turning the channel's egress into
> unfiltered web access for *both* `conn.open` and `channel.http`/`channel.send`.
> This is never the right answer for a real channel (a channel talks to one
> provider); it exists only as an escape hatch and, like a broad suffix, is an
> operator-review responsibility, not a code-enforced limit.

Phase-1 tests run against a loopback pin, so the primitive and its test suite land
independent of any of this.

## Manifest: transport capability declaration

The `channel` block gains an optional `transports` array — **advisory and
validated, never the source of truth for endpoints** (URLs are runtime values:
`resume_gateway_url`, per-number callbacks, `/sync` tokens). Values:
`poll`, `webhook`, `persistent`.

```json
"channel": { "type": "discord", "handler": "channel.qjs",
             "transports": ["persistent"] }
```

`--check` uses it to fail-closed early: a `persistent` channel on a libcurl built
without WS/WSS is refused at activation with a clear message, rather than
throwing at the first `conn.open`. Absent = unconstrained (Telegram declares
nothing and is unaffected). It documents intent for the next handler author and
lets C skip wiring conn machinery a channel will never use.

## Non-goals (v1)

- Binary ws frames / `permessage-deflate` compression — text/json only.
- `raw` framing — specified above, implemented when first needed.
- Sub-second timers, multiple concurrent conns beyond the small cap.
