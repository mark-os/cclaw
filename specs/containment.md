# Containment — levels, enforcers, shipped rows (as built)

A containment says how an agent's tool children are contained. It carries no
authority: which tools an agent may call and which hosts it may reach are
`grants`, never the containment. Complements [security.md](security.md),
[egress-filter.md](egress-filter.md) (the proxy's host rules) and
[portability.md](portability.md) (what each kind of host can enforce).

## The model

A containment is a row of the `containments` table, and `agents.containment`
names one (looked up by name; the column default is `standard`):

| column | values | meaning |
|---|---|---|
| `files` | `private` > `confined` > `open` | what the child can open |
| `network` | `none` > `proxied` > `advisory` > `direct` | where its packets go |
| `nproc`, `cpu_sec` | integer, `0` = no cap | `RLIMIT_NPROC`, `RLIMIT_CPU` |

Shipped rows, restored to these values at every start (`CONTAIN_SHIPPED`,
`src/sandbox.c`, written by `db_ensure_schema`). An operator may add rows (SQL
or the explorer); those are left alone. No agent-facing verb writes the table.

| name | files | network | nproc | cpu_sec |
|---|---|---|---|---|
| `standard` (default) | private | proxied | 256 | 0 |
| `offline` | private | none | 64 | 120 |
| `basic` | confined | advisory | 256 | 0 |
| `none` | open | direct | 0 | 0 |

**A level is a promise.** For each call the parent picks the strongest
enforcer this host has for each level, for the vehicle at hand; if no enforcer
can keep a level, the call is refused before anything forks, and the tool
result says which containment, which level, what the host lacks and a shipped
containment it can keep. A level is never silently weakened, and a name with no
row is refused, not read as a default.

### Enforcers

| | enforcer | what it is |
|---|---|---|
| files | `FE_MOUNTNS` | the user + mount namespace sandbox (`contain_enter`) |
| | `FE_CODE` | the path check in cclaw's own file tools and js globals |
| | `FE_NONE` | nothing (`open`) |
| network | `NE_NETNS` | the empty network namespace the mount namespace brings; the proxy socket is the only way out |
| | `NE_CODE` | js's curl dials the egress proxy's socket itself |
| | `NE_ENV` | the proxy runs on the host's loopback and the proxy variables are set; nothing else |
| | `NE_NONE` | nothing |

### The planner

`contain_plan()` (`src/sandbox.c`) is one pure function: (row, vehicle,
`contain_caps()`) → plan or refusal. `run_tool_req_init` calls it for every
child call; `--doctor` and the startup warning call it per agent.

| files | file | js | shell |
|---|---|---|---|
| `private` | MOUNTNS, else refuse | MOUNTNS, else refuse | MOUNTNS, else refuse |
| `confined` | MOUNTNS, else CODE | MOUNTNS, else CODE | MOUNTNS, else refuse |
| `open` | NONE | NONE | NONE |

| network (no mount namespace) | file | js | shell |
|---|---|---|---|
| `none` | NONE | CODE (http_request refuses) | refuse |
| `proxied` | NONE | CODE | refuse |
| `advisory` | NONE | CODE | ENV (needs the backend's loopback proxy) |
| `direct` | NONE | NONE | NONE |

With the mount namespace the network enforcer is always `NETNS`: there the
proxy is the only route out, which keeps `none` and `proxied` and keeps
`advisory` more strongly than it promises (`--doctor` says so). `direct` cannot
be combined with the mount namespace and is refused. The file vehicle opens no
socket — it is cclaw's own file code — so no network level needs an enforcer
there. js is cclaw's code too, and QuickJS exposes no socket primitive (no
`os`/`std` modules): its one way out is `http_request`.

What this means for the shipped rows:

- On a host with user namespaces, `standard`, `offline` and `basic` all run in
  the namespace sandbox, and `none` runs with no boundary.
- On a host without them, `standard` and `offline` are refused for every
  vehicle; under `basic` the file tools and js run (path check in code, proxy
  forced in code) and **the shell is refused** — it needs a kernel enforcer
  for `confined`, and Landlock is not built yet.

### The child

The plan rides the `--run-tool` wire (levels, limits, enforcers). The child
applies exactly it (`sandbox_child_setup`) and exits 126 if any step fails; it
never picks a weaker enforcer.

`contain_caps()` (`src/contain.h`) is probed once per process by fork-and-try
— `MOUNTNS` (unshare user, mount, IPC, PID and network namespaces, map the user,
make the mount tree private: the first step that needs a capability, which is
what Ubuntu's AppArmor restriction denies), `NETNS` (a network namespace with
`lo` up; reported now, planned with in 4b), `SHIM` (the backend can serve the
loopback proxy; always on Linux) — cached in memory, never persisted.
`CCLAW_CONTAIN_CAPS_MASK`, a test and diagnostic knob read only there, is ANDed
in: it can only take capabilities away. `make CONTAIN=none` reports none.

## Under every plan

Empty-env exec, fd hygiene, `PR_SET_PDEATHSIG`, the row's rlimits when
non-zero, and `HOME`/`PATH`/`TMPDIR`:

- **`HOME`** is the agent workspace under any files enforcer: rw, persistent,
  the one directory the agent owns, so `pip install --user`, `cargo install`
  and `npm -g --prefix $HOME` land there and are on `PATH` next call. Under
  `open` it is the invoking user's home from the passwd database (the child
  starts from an empty environ, so there is nothing to inherit).
- **`PATH`** is `$HOME/.local/bin`, `$HOME/.cargo/bin`, `$HOME/bin`, then the
  system dirs including `/usr/local/bin` and `/usr/local/go/bin`.
- **`TMPDIR`** is `/tmp` — in the namespace a bind of the agent's scratch dir;
  under `FE_CODE` the scratch dir itself.

Additions: shell and js get `CCLAW_PROXY_SOCK` and, under `NETNS`/`ENV`, the
proxy variables (`HTTP_PROXY`, `HTTPS_PROXY`, `ALL_PROXY`, `NO_PROXY`, and
lowercase); the shell also `CCLAW_CONNECT` and `GIT_SSH_COMMAND`
([shell-networking.md](shell-networking.md)); js gets `CCLAW_LLM_SOCK` unless
the network is `none`; a shell child gets the `CCLAW_SECRET_*` values its
command references.

**`no_new_privs`** is set on every child except files `open` with network
`direct` (containment `none`), which keeps the operator's own reach, `sudo`
included.

**No row sets `RLIMIT_AS`**: it caps address space, which every modern runtime
reserves far beyond its footprint (Go arenas, V8 cages). NPROC is the
fork-bomb backstop; a CPU cap punishes legitimate builds, so only `offline`
has one.

**The CLI's CWD** reaches the child only under files `open`: there it inherits
the user's CWD (`cclaw` in a repo operates on that repo); under `--daemon`, and
under every other files level, the child starts in the workspace.

## files `private`: the mount namespace

A tmpfs root, `pivot_root`, and binds: the system dirs (`/bin /usr /lib /lib64
/etc /dev /sbin /opt`, plus `/proc` for the shell, which has a PID namespace)
read-only all the way down — every submount a recursive bind brings along is
remounted read-only on its own, and `/dev/shm`, `/dev/mqueue` and `/dev/pts`
get instances of their own in the child's own IPC namespace; the workspace rw;
`/tmp` a bind of the agent's scratch dir (`<tmp_root>/cclaw-<uid>/<agent>/`,
created 0700 by the parent, persistent across calls on purpose, never the
host's shared `/tmp`); path grants ro or rw, planned shallow→deep so a nested
grant shadows its parent, a grant naming a single file binding just that file.
Everything else does not exist. Any of these failing refuses the call.

## files `confined` without a namespace: the path check

`FE_CODE` (`contain_path_check`) runs before every open, stat, list or write in
the file tools, js `fs.*`, `http_request`'s `save_to`, `LLM()`'s `images` and a
js `filename`. It allows what the mount namespace would bind: workspace and
scratch rw, grants with their ro/rw (the deepest matching grant decides), the
system dirs read-only. The path is resolved first — `realpath` of the deepest
existing ancestor, plus the part not yet created, where a `..` is refused — so
a symlink in the workspace cannot point out. `.cclaw_key` and `cclaw.db*` are
refused by resolved path and by inode (a hard link has no other name).
Differences from the namespace, all narrower or equal except the last: `/proc`
is not readable, `/` and other ungranted parents cannot be listed, a stat of an
ungranted path is refused rather than "not found", and it is a check, not a
kernel boundary — two concurrent calls of the same agent racing a symlink swap
could pass it, which is what Landlock (4b) closes as a backstop.

## Fail-closed rule

A refusal at dispatch is the normal outcome on a host that cannot keep a
level; a child that cannot apply its plan exits 126. Neither degrades.
`--no-containment` sets `CCLAW_CONTAINMENT=none` for a CLI run; `cclaw
--doctor` `[layers]` prints, per agent, the containment, its levels and each
vehicle's enforcers or refusal, and the startup warning names a shipped
containment the host can keep.

`create_agent` and `update_agent` refuse a containment with no row, and one
looser than the caller's in any single column (files, network, nproc, cpu_sec;
a limit of 0 is the loosest).

`cclaw.db` and `.cclaw_key` are out of reach under every files level but
`open`, and a `read_path`/`write_path` grant naming either, or a directory
holding them, is refused at grant time (`grant_path_hits_db`). Agent
self-inspection is `db_query`, which runs in the trusted parent.

## Interaction with other axes (do not conflate)

- **Authority** = `grants`. Withholding `shell_exec` is the strongest lockdown;
  an empty host-grant set is deny-all egress at the proxy.
- **Escalation** = `approvals`.
- The containment must never gate a capability.

## Tests

`test/test_containment.c` (the planner, every refusal rule),
`test/test_contain_code.c` (`basic` with capabilities masked, through the real
`--run-tool` child: the path check, js through the proxy, refusals),
`test/test_contain.c` (the probe and the mask), the namespace suites
(`test_shell_namespace.c`, `test_run_tool_escape.c`, `test_sandbox_mounts.c`,
`test_sandbox_key_mask.c`, `test_integration_shell_proxy*.c`), and the per-column
cap in `test/test_agent_define.c`.

## Not yet (step 4b)

Landlock (files `confined` for the shell, and a backstop under `FE_CODE`),
seccomp (network `none` without a namespace), and a network namespace without
a mount namespace (`proxied` for the shell, and js, under files `open`).
