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

Three profiles, deliberately: no sandbox, sandbox, sandbox-without-network.
None of them is merely a tuning preset.

| Policy                  | host             | standard (default) | restricted |
|-------------------------|------------------|--------------------|------------|
| namespace sandbox       | **none**         | required           | required   |
| env                     | inherit+scrub    | clean allowlist    | clean allowlist |
| network                 | direct           | proxy              | none (no proxy sock) |
| CWD mount (CLI)         | rw (host fs)     | **no**             | no         |
| workspace mount         | rw (host fs)     | rw                 | rw         |
| `$HOME`                 | user's real home | workspace          | workspace  |
| `/tmp`                  | host `/tmp`      | private scratch bind | private scratch bind |
| rlimits NPROC / CPU     | none             | 256 / none         | 64 / 120s  |
| `RLIMIT_AS`             | **never**        | **never**          | **never**  |

**Containment is about reach, not resources.** `standard` mounts no CWD, so an
agent sees its own workspace and whatever `read_path`/`write_path` grants add,
and nothing of the user's files or another agent's workspace. Widening
visibility is the grant system's job (and therefore the approval flow's), never
a side effect of picking a looser profile. `host` is the one profile that sees
the user's files, because it establishes no namespace at all.

**Why `standard` has NPROC but no CPU cap.** The NPROC cap is not policy, it is
the fork-bomb backstop: on a 128 MB target one bad generated shell line
otherwise takes the daemon down with it. A per-process CPU cap punishes nothing
real — legitimate builds brush it — so it is gone. (`standard` absorbed the
former `trusted` profile, whose mount set was already identical; the merge cost
`trusted`'s env inheritance, which was a hygiene regression acquired as a side
effect rather than anything anyone chose.)

**`restricted` means no packets, ever.** The child gets no proxy socket and an
empty netns, so the denial is kernel-enforced rather than a `grants` row —
not even a promoted tool with declared hosts egresses. Parent-side tools
(memory, `db_query`, the LLM loop itself) are untouched, so a `restricted`
note-taking agent converses, remembers, and runs local programs fine. File
grants to a `restricted` agent remain legal: the profile constrains packets,
not files.

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

**`/tmp` is a bind of the agent's own scratch directory on the host**, not a
directory on the (deliberately tiny) root tmpfs and not a tmpfs we size
ourselves. Toolchains write real volume there — `cc` intermediates, configure
scripts, npm staging — and a shared 1 MB root failed every build with ENOSPC.

Binding a host directory means temp storage inherits whatever the host already
does, tmpfs or disk, plus the host's own `tmpfiles.d` cleaner — rather than this
code inventing a size policy that cannot be right for both a 128 MB SoC and a
16 GB server. (An earlier cut mounted a tmpfs at a percentage of RAM. That was
backwards: it put the *most* write-heavy workload in RAM, and on a 128 MB
Pogoplug it yielded a 64 MB `/tmp`, reintroducing the ENOSPC it was added to
fix. Most toolchain bulk lands under `$HOME` anyway, which is the workspace.)

Layout is `<tmp_root>/cclaw-<uid>/<agent>/`, created by the *parent* — `tmp_root`
defaults to the host's `/tmp`. The uid keeps two users running cclaw on one box
from colliding on a 0700 directory; it is not what stops squatting, since `/tmp`
is mode 1777 and anyone can pre-create a name. That is the ownership check: every
level is created 0700 and, if it already exists, must be a real directory owned
by us with mode 0700 and not a symlink, or the call fails and the child simply
gets no scratch bind. It is never the host's *shared* `/tmp` — that would hand
the child every other process's scratch, ssh-agent and gpg-agent sockets
included.

It **persists across tool calls** on purpose: `./configure` in one call and
`make` in the next need the same `/tmp`, package caches want to persist, and the
tool-call boundary is invisible to the agent, so wiping it presents as "my files
vanished" rather than anything diagnosable.

The namespace root lives under the same tree (`.ns/<pid>`) so the parent can reap
abandoned ones without tracking pids: a live one holds a mount and refuses
`rmdir`, a dead one is empty and goes.

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
  `bootstrap` level, and no `trusted` level any more — both are treated like
  any other unknown value (→ standard, which is strictly tighter than the old
  `trusted`). A DB stamped before schema v34 has its `trusted` rows rewritten
  to `standard` by the forward patch. The seeded default agent (`Assistant`)
  is `standard`.
- **clean allowlist env**: `clearenv()`, then only PATH, TMPDIR,
  `CCLAW_PROXY_SOCK`, and `CCLAW_SECRET_*` injections. Replaces the
  blacklist scrub used by `host` (which is inherit-minus-blacklist:
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
`pivot_root` fails, the child prints an error and exits 126 instead of
degrading — a runtime failure must
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
profile may set `RLIMIT_AS`, and every unknown value (`trusted` and `bootstrap`
included) falls through to `standard`. The state-file mask has its own suite,
`test/test_sandbox_key_mask.c` — every masking assertion there is paired with a
positive control read of a decoy file in the same granted directory.

## Non-goals

- seccomp syscall filtering — maintenance tax; namespaces+proxy already cover
  the realistic threats; revisit only with a concrete bypass in hand.
- Container/VM isolation — the shell still runs as the user's uid under the
  shared kernel; that boundary is out of scope for cclaw itself.
