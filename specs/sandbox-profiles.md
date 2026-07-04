# Sandbox Profiles — Per-Agent Containment Bundles (as built)

`agents.sandbox_profile` selects a **sandbox policy bundle** — containment only.
It carries no authority: which tools an agent may call and which hosts it may
reach are governed by the `grants` table, never by the level. Complements
specs/security.md (secret handling, defense-in-depth) and specs/egress-filter.md
(proxy host rules).

Resolution happens in ONE place: `sandbox_policy_from_profile()`
(`src/sandbox.c`), called from `agent_setup_init()` (`src/agent_setup.c`),
which fills the `SandboxProfile` used by every sandboxed tool tier (shell,
web, js, file — all via the `--run-tool` broker, `src/run_tool.c`).

## Policy bundles

| Policy                  | host             | trusted          | standard (default) | restricted |
|-------------------------|------------------|------------------|--------------------|------------|
| namespace sandbox       | **none**         | required         | required           | required   |
| env                     | inherit+scrub    | inherit+scrub    | clean allowlist    | clean allowlist |
| network                 | direct           | proxy            | proxy              | none (no proxy sock) |
| CWD mount (CLI)         | rw (host fs)     | rw               | no                 | no         |
| workspace mount         | rw (host fs)     | rw               | rw                 | ro         |
| rlimits (NPROC/AS/CPU)  | none             | none             | generous           | tight      |

- **Unknown or NULL values fall through to `standard`.** There is no
  `bootstrap` level — the string is treated like any other unknown value
  (→ standard). The seeded default agent (`Assistant`) is `trusted`.
- **clean allowlist env**: `clearenv()`, then only PATH, TMPDIR,
  `CCLAW_PROXY_SOCK`, and `CCLAW_SECRET_*` injections. Replaces the
  blacklist scrub used by host/trusted (which is inherit-minus-blacklist:
  names containing API_KEY/APIKEY/TOKEN/SECRET/PASSWORD/CREDENTIALS are
  dropped, `CCLAW_SECRET_*` re-injected as the deliberate channel).
- **network none** (`restricted`): no `CCLAW_PROXY_SOCK` in the child, no
  proxy started; the empty netns then has zero egress.
- **grant-path bind mounts**: `read_path`/`write_path` grants add bind
  mounts on top of the bundle (`src/sandbox.c` `sandbox_apply_namespace`);
  they extend *visibility*, chosen per-agent via the approval flow
  (`request_config` → `apply_grant`), and are refreshed per dispatch batch.
- Sanitizer builds skip `RLIMIT_AS` (see AGENTS.md — ASan shadow VA).

## Fail-closed rule

Every level except `host` **requires** the namespace. If `unshare`/
`pivot_root` fails (or the `restricted` read-only remount fails), the child
prints an error and exits 126 instead of degrading — a runtime failure must
not grant what only `host` may grant. `host` never attempts the sandbox; it
is the explicit opt-out, per-agent via `agents.sandbox_profile` or session-wide
via `--trust-host` (sets `CCLAW_SANDBOX_PROFILE=host`). Sandbox on/off is derived
from the level; there is no separate sandbox knob.

## Interaction with other axes (do not conflate)

- **Authority** = `grants` (kind/value, per-tool `approval_mode`,
  `expires_at`). Withholding a `tool` grant for `shell_exec` is the strongest
  lockdown; an empty host-grant set is deny-all egress at the proxy.
- **Escalation** = `approvals` (park/resolve, `rerun`/`apply`).
- The profile must never gate a capability. If a change makes tool/secret
  access depend on `sandbox_profile`, it belongs on `grants` instead.

## Test coverage

Integration tests exercise the namespace, proxy-deny, and static-mount paths
(`test/test_shell_namespace.c`, `test_integration_shell_proxy*.c`,
`test_sandbox_mounts.c`). The profile→bundle mapping itself
(`sandbox_policy_from_profile`) has no direct unit test yet.

## Non-goals

- seccomp syscall filtering — maintenance tax; namespaces+proxy already cover
  the realistic threats; revisit only with a concrete bypass in hand.
- Container/VM isolation — the shell still runs as the user's uid under the
  shared kernel; that boundary is out of scope for cclaw itself.
