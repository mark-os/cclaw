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
| CWD mount (CLI)         | rw (host fs)     | **no**           | no                 | no         |
| workspace mount         | rw (host fs)     | rw               | rw                 | rw         |
| `$HOME`                 | user's real home | workspace        | workspace          | workspace  |
| `/tmp` tmpfs            | host `/tmp`      | 50% of RAM       | 50% of RAM         | 25% of RAM |
| rlimits NPROC / CPU     | none             | none             | 256 / 1800s        | 64 / 120s  |
| `RLIMIT_AS`             | **never**        | **never**        | **never**          | **never**  |

**Containment is about reach, not resources.** `trusted` means "spend what you
need", never "see more" — it mounts no CWD, so a trusted agent sees its own
workspace and whatever `read_path`/`write_path` grants add, and nothing of the
user's files or another agent's workspace. Widening visibility is the grant
system's job (and therefore the approval flow's), never a side effect of picking
a looser profile. `host` is the one profile that sees the user's files, because
it establishes no namespace at all.

**`$HOME` is set under every profile.** Sandboxed profiles point it at the agent
workspace: it is bind-mounted rw at its own absolute path and persists across
tool calls, so `pip install --user`, `cargo install`, and `npm -g --prefix
$HOME` land inside the one directory the agent owns and are on `PATH` next call
— an agent can bootstrap its own toolchain without a grant. `host` reads the
invoking user's home from the passwd database (the `--run-tool` child starts
from an empty environ, so there is nothing to inherit). This is also why no
sandboxed profile mounts the workspace read-only any more: `$HOME` has to be
writable or every toolchain fails on its cache.

**`PATH`** is the agent's workspace-local bin dirs (`$HOME/.local/bin`,
`$HOME/.cargo/bin`, `$HOME/bin`) followed by the system dirs including
`/usr/local/bin` and `/usr/local/go/bin`. It was `/bin:/usr/bin`, which made
every real toolchain invisible — `node`, `go`, `cargo` and `rustc` all install
outside those two directories.

**`/tmp` is its own tmpfs**, not a directory on the (deliberately tiny) root
tmpfs. Toolchains write real volume there — npm unpacking tarballs, cargo and
rustc temporaries, `cc` intermediates, go's build cache — and a shared 1 MB root
failed every build with ENOSPC. The size is a percentage (tmpfs's own syntax) so
it scales from a 128 MB SoC to a 16 GB server without probing meminfo, and tmpfs
charges only what is written, so it is a ceiling and not a reservation.

**No profile sets `RLIMIT_AS`.** It caps *address space*, not resident memory,
and every modern runtime reserves VA far beyond its footprint — Go's allocator
arenas, V8's heap cages, LLVM. A cap loose enough to be safe bounds nothing; a
cap tight enough to bound anything refuses to start `go`, `node`, or `rustc`
before they do a byte of work. NPROC and CPU bound a runaway; a cgroup memory
limit is the right tool if a real memory ceiling is ever wanted.

**`host` and the CWD.** `host` runs no namespace, so nothing is mounted and no
`chdir` is implied. In CLI mode the child inherits the user's CWD — that is the
point of `host`, since `cclaw` invoked in a repo should operate on that repo.
Under `--daemon` there is no meaningful user CWD, so the child starts in the
agent workspace instead.

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
  A grant may name a **single file** or a directory (`bind_path_into` creates
  a matching mount point for either), so an agent that needs one file can be
  given exactly that instead of its parent. Mounts are planned shallow→deep,
  so a file grant nested inside a granted directory correctly shadows it.
  A grant on a path that does not exist yet cannot be mounted — to create
  files, grant the directory.
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
`test_sandbox_mounts.c`). The profile→bundle mapping is pinned field-by-field in
`test/test_sandbox_profile.c`, which also enforces two invariants directly: no
profile may set `RLIMIT_AS`, and every sandboxed profile must leave the
workspace writable (because `$HOME` points at it).

## Non-goals

- seccomp syscall filtering — maintenance tax; namespaces+proxy already cover
  the realistic threats; revisit only with a concrete bypass in hand.
- Container/VM isolation — the shell still runs as the user's uid under the
  shared kernel; that boundary is out of scope for cclaw itself.
