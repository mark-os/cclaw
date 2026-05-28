# CClaw Security Review & Shell Networking Proposal

## Current Security Model (Summary)

| Layer | Mechanism | Protects Against |
|-------|-----------|-----------------|
| Process isolation | fork+exec, ephemeral (one turn) | state leakage between turns |
| Filesystem | namespace (CLONE_NEWNS): remount / ro, workspace rw | reading other agents' data, system files |
| Resources | setrlimit (AS/CPU/NOFILE) | runaway memory, CPU, fd exhaustion |
| Env stripping | unset CCLAW_*, API keys before exec | credential leakage to shell children |
| Network | namespace (CLONE_NEWNET) + iptables REDIRECT → proxy | arbitrary outbound, credential exposure |
| Secrets | ChaCha20-Poly1305 in daemon.db, injected at fork | at-rest exposure |

## Primary Sandbox: Linux Namespaces

Namespaces replace landlock as the primary sandbox mechanism. Advantages:

- **Single mechanism** for filesystem + network (no split between landlock versions)
- **Transparent proxy** possible (iptables REDIRECT in netns — impossible with landlock)
- **Wider kernel support** — `CONFIG_USER_NS` + `CONFIG_NET_NS` available on virtually all Linux kernels since 3.8; no special security module needed
- **Pogoplug compatible** — user namespaces likely available even where landlock isn't compiled in
- **Already prototyped** — `sandbox/unshare-fallback` branch has working `unshare(CLONE_NEWUSER|CLONE_NEWNS|CLONE_NEWNET)` + remount code

### Daemon Startup Checks

On startup, daemon verifies namespace support and reserves capacity:

```c
// 1. Check user namespaces enabled
//    /proc/sys/user/max_user_namespaces must be > 0
int max_userns = read_proc_int("/proc/sys/user/max_user_namespaces");
if (max_userns == 0) fatal("user namespaces disabled");

// 2. Enforce agent limit at half of kernel max
//    V3 says max 10 system-wide; each shell_exec uses ~3 namespaces (user+net+mnt)
//    So 10 agents × 3 ns = 30 namespaces needed
int cclaw_limit = MIN(max_userns / 2, V3_MAX_SYSTEM_WIDE * 3);
if (cclaw_limit < V3_MAX_SYSTEM_WIDE * 3)
    warn("user_ns limit %d constrains max agents to %d", max_userns, cclaw_limit / 3);

// 3. Verify unshare works (test fork+unshare+exit)
if (!test_unshare()) fatal("unshare(CLONE_NEWUSER) failed — check /proc/sys/kernel/unprivileged_userns_clone");
```

Relevant sysctls:
- `/proc/sys/user/max_user_namespaces` — kernel limit (default 65536 on most distros)
- `/proc/sys/kernel/unprivileged_userns_clone` — must be 1 (Debian/Ubuntu may default to 0)

### Shell Child Sandbox (per shell_exec)

```c
// In shell_exec child, after fork, before exec:

// 1. Namespace isolation
unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWNET);

// 2. Write uid/gid maps (required for CLONE_NEWUSER)
write_file("/proc/self/uid_map", "0 <parent_uid> 1");
write_file("/proc/self/setgroups", "deny");
write_file("/proc/self/gid_map", "0 <parent_gid> 1");

// 3. Filesystem: remount / read-only, workspace read-write
mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY | MS_REC, NULL);
mount(workspace, workspace, NULL, MS_BIND, NULL);
mount(NULL, workspace, NULL, MS_REMOUNT | MS_BIND, NULL);  // rw
// Also: /tmp/cclaw-<pid>/ for scratch

// 4. Network: loopback up + redirect all TCP to proxy
//    Parent's proxy thread listens on host netns, connected via veth pair
//    OR simpler: proxy listens on child's loopback (inherited before unshare)
setup_loopback();
iptables_redirect_all_tcp(proxy_port);

// 5. Env: strip secrets, set PATH
setenv("PATH", "/bin:/usr/bin", 1);
unsetenv("CCLAW_INJECTED_API_KEY");
unsetenv("HOME");
// ... all CCLAW_* vars

// 6. Resource limits
setrlimit(RLIMIT_AS, 256MB);
setrlimit(RLIMIT_CPU, 300s);
setrlimit(RLIMIT_NOFILE, 64);

// 7. Exec
execl("/bin/sh", "sh", "-c", command, NULL);
```

## Transparent Credential Proxy

### How It Works

```
shell child: connect("api.github.com", 443)
    → kernel (netns iptables): REDIRECT to 127.0.0.1:<proxy_port>
    → proxy: getsockopt(SO_ORIGINAL_DST) → "api.github.com:443"
    → proxy: check allowlist → allowed?
    → proxy: present MITM cert for api.github.com (signed by per-process CA)
    → shell child: TLS handshake (trusts our CA via SSL_CERT_FILE)
    → proxy: sees plaintext HTTP request
    → proxy: lookup credential_mappings for api.github.com + path
    → proxy: inject Authorization header
    → proxy: open real TLS connection to api.github.com:443
    → proxy: forward modified request, relay response
```

The shell child never knows. It made a normal `connect()` syscall. curl, git, pip, wget — all work transparently.

### Proxy Lifecycle

- **Born**: when agent process starts (thread in agent process)
- **Dies**: when agent process exits
- **Listens**: loopback only, ephemeral port
- **CA**: generated fresh per process, written to `/tmp/cclaw-<pid>/ca.pem`
- **Secrets**: held in memory (decrypted by daemon, injected via env at fork)

### Credential Mapping (daemon.db)

```sql
CREATE TABLE credential_mappings (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    agent_name TEXT NOT NULL,
    secret_name TEXT NOT NULL,       -- references kv key in daemon.db
    host_pattern TEXT NOT NULL,       -- "api.github.com", "*.example.com"
    path_pattern TEXT DEFAULT '',     -- "/api/v1/write" (empty = all paths)
    location TEXT NOT NULL,           -- "bearer", "header:X-API-Key", "query:token", "basic:<user>"
    created_at INTEGER DEFAULT (unixepoch())
);
```

### Allowlist Enforcement

Proxy denies connections to hosts not in agent's `allowed_hosts[]`. This replaces landlock v4 network rules entirely. Enforcement is at the proxy level — the shell child physically cannot reach any host the proxy doesn't forward to.

### DNS Interception + Domain Cache

All protocols (HTTP, SSH, WebSocket, etc.) use domain-based allowlisting uniformly via DNS interception:

```
iptables -t nat -A OUTPUT -p udp --dport 53 -j REDIRECT --to-port <dns_proxy_port>
iptables -t nat -A OUTPUT -p tcp -j REDIRECT --to-port <tcp_proxy_port>
```

Flow:
```
shell: ssh good.com
  → DNS query for good.com (UDP 53) → redirected to proxy DNS handler
  → proxy resolves good.com → 5.6.7.8
  → proxy caches {5.6.7.8 → good.com}
  → proxy returns 5.6.7.8 to shell
  → shell: connect(5.6.7.8:22)
  → iptables REDIRECT → proxy TCP handler
  → proxy: dest=5.6.7.8:22, lookup cache → "good.com"
  → proxy: check "good.com" against allowlist → allowed
  → tunnel raw TCP through (SSH handshake works)

shell: ssh evil.com
  → DNS query → proxy resolves → 9.9.9.9, caches {9.9.9.9 → evil.com}
  → shell: connect(9.9.9.9:22)
  → proxy: lookup cache → "evil.com" → NOT in allowlist → DENIED (RST)
```

This means:
- **HTTP/HTTPS**: domain from SNI/Host header (primary) or DNS cache (fallback)
- **SSH, raw TCP, WebSocket**: domain recovered from DNS cache
- **Hardcoded IPs** (no DNS): proxy has no cached domain → deny by default (only allowlisted domains work, not raw IPs)
- **DNS exfiltration**: blocked — all DNS goes through our proxy, which only resolves and caches (doesn't forward to attacker-controlled nameservers)

The DNS handler is a simple UDP relay: receive query from child, forward to system resolver, cache the answer (domain→IP mapping), return to child. ~30 lines of C.

### PII / Sensitive Data

Same mechanism. Store PII as named secrets in daemon.db, create credential_mappings for the hosts+paths that need them. Proxy injects transparently. Agent references by name ("use my github token"), never sees the value.

### Network Policy Modes

Two modes, same enforcement (proxy + in-process `http_check_policy`):

| Mode | Config | Behavior |
|------|--------|----------|
| Default-deny (restrictive) | `allowed_hosts: ["api.github.com", "pypi.org"]` | Only listed hosts reachable |
| Default-allow (permissive) | `allowed_hosts: []` | All hosts reachable, except `blocked_hosts` |

Both modes always block RFC1918/loopback/link-local (SSRF protection).

### Blocklist Loading

For default-allow mode, a global blocklist provides the safety net:

```
# daemon.db kv
blocked_hosts_file = /etc/cclaw/blocklist.txt
```

File format: plain text, one domain per line. Compatible with:
- abuse.ch URLhaus domain-only exports (~3K-8K domains)
- Pi-hole domain-only format
- Any one-domain-per-line file

```
# /etc/cclaw/blocklist.txt
malware-c2.example.com
phishing.evil.net
known-bad.cryptominer.org
```

Implementation:
- Loaded into hash set at daemon startup
- ~200KB for 10K domains (realistic security blocklist)
- Passed to agent processes (same `HttpPolicy.blocked_hosts` field)
- Proxy uses same hash set for shell child enforcement
- If list exceeds memory budget on constrained devices: fall back to SQLite `blocked_domains` table with indexed lookup

## Threat Model

| Threat | Mitigation |
|--------|-----------|
| Shell leaks API key via `echo $KEY` | Key never in shell env — proxy injects at network boundary |
| Shell exfiltrates data to attacker host | Allowlist blocks unlisted hosts (proxy denies CONNECT) |
| Shell bypasses proxy via direct socket | Impossible — iptables REDIRECT in netns captures all TCP |
| Prompt injection extracts credential | Credential never in LLM context — agent references by name |
| Shell reads filesystem secrets | `/` is read-only; workspace has no secrets; daemon.db unreachable |
| Shell escapes to host network | CLONE_NEWNET isolates; no veth to host (proxy on child loopback) |
| Rogue shell reads CA private key | CA key in agent memory only (parent process); not on filesystem |
| DNS exfiltration | Proxy can intercept DNS too (redirect UDP 53); or block and provide DNS via proxy |

### Accepted Risks

| Risk | Rationale |
|------|-----------|
| Agent C process has secrets in memory | Ephemeral process; same trust model as any API client |
| Shell can log response bodies | Response data is the point of the API call |
| Namespace unavailable (rare) | Graceful fallback: run unsandboxed with warning (single-user system) |

## Comparison: Namespaces vs Landlock

| Aspect | Landlock | Namespaces + Proxy |
|--------|---------|-------------------|
| Filesystem sandbox | v2+ (5.13) | CLONE_NEWNS + remount (3.8+) |
| Network sandbox | v4 only (6.7+) | CLONE_NEWNET (3.8+) |
| Transparent proxy | Impossible | iptables REDIRECT |
| Credential injection | Impossible | MITM via per-process CA |
| Kernel requirement | CONFIG_SECURITY_LANDLOCK | CONFIG_USER_NS (standard) |
| Pogoplug (ARMv5) | Not compiled in | Likely available |
| Bypass resistance | Kernel-enforced | Kernel-enforced (netns + iptables) |
| Complexity | Low (few syscalls) | Medium (ns setup + proxy thread) |
| Works with all binaries | Yes | Yes |

**Decision**: Namespaces as primary. Landlock removed from design. Graceful fallback to unsandboxed if namespaces unavailable (log warning).

## Spec Impact

### Remove/Replace

- V22, V22a, V37: landlock references → replace with namespace sandbox
- V47: keep (env stripping still applies)
- T84, T102, T207: landlock tasks → namespace tasks

### New Invariants

- V82: ∀ `shell_exec` child → `unshare(CLONE_NEWUSER|CLONE_NEWNS|CLONE_NEWNET)` before exec; `/` remounted ro; workspace bind-mounted rw; all TCP redirected to credential proxy via iptables; graceful fallback if namespaces unavailable
- V83: ∀ credential proxy → per-agent-process thread, loopback, ephemeral port; MITM CA per-process; enforces `allowed_hosts` allowlist; injects credentials by host+path matching from `credential_mappings` table; `getsockopt(SO_ORIGINAL_DST)` to learn real destination
- V84: ∀ credential mapping → requires admin approval (V54); stored in daemon.db; agent references secrets by name, never sees values in context or env
- V85: ∀ daemon startup → verify `/proc/sys/user/max_user_namespaces` > 0; verify `unshare(CLONE_NEWUSER)` succeeds; enforce max concurrent agents ≤ `max_user_namespaces / 6` (each shell_exec uses ~3 ns pairs); warn if constrained

### New Tasks

- T210: namespace sandbox for shell_exec (replace landlock)
- T211: credential proxy thread (civetweb or raw sockets, MITM CA, allowlist, injection)
- T212: credential_mappings table + daemon management
- T213: iptables REDIRECT setup in child netns
- T214: daemon startup namespace capability check
- T215: per-process CA generation (ephemeral, destroyed on exit)
- T216: integration test: shell curl through proxy with injected credentials
- T217: integration test: shell cannot reach unlisted host
- T218: integration test: shell cannot read filesystem outside workspace

## Implementation Notes

### Proxy Thread vs Separate Process

Proxy runs as a **thread in the agent process** (not a separate process):
- Shares agent's memory (has access to decrypted secrets)
- Dies automatically when agent exits
- No IPC needed for credential lookup
- Listens on child's loopback (set up before unshare, or via veth)

### iptables in Unprivileged Namespace

After `unshare(CLONE_NEWUSER|CLONE_NEWNET)`, the process is root in its own user+net namespace. It can run iptables rules freely — no real root needed on the host.

### Proxy Port Inheritance

Option A: Proxy listens on host loopback, child connects via veth pair.
Option B: Proxy binds before fork, child inherits the listening socket fd, proxy thread serves from parent. Child's iptables redirects to that inherited port.

**Option B is simpler** — no veth setup. Parent binds proxy socket, forks shell child, child inherits the fd (or connects to parent's loopback port which is accessible before CLONE_NEWNET takes effect if we order operations carefully).

Actually simplest: **parent starts proxy on host loopback:9123, child's netns has a veth pair with one end in host netns**. Child's iptables redirects all TCP to the veth gateway IP. This is the standard Docker networking model.

OR even simpler: **don't use CLONE_NEWNET for the proxy path**. Use CLONE_NEWNS (filesystem) + iptables in the *host* netns with owner-match (`-m owner --uid-owner <child_uid>`). But this requires real UID separation...

**Simplest viable**: proxy listens before fork. Child does NOT unshare network (keeps host netns). Child's iptables uses `-m owner --pid-owner` (deprecated) or we just rely on `HTTP_PROXY`/`HTTPS_PROXY` env vars + landlock-v4-if-available as belt-and-suspenders. 

**Recommended**: Use CLONE_NEWNET + veth pair. It's ~20 lines of setup and gives bulletproof network isolation. The proxy lives in the host netns, child's veth connects only to it.
