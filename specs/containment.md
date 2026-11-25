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
| `network` | `none` > `firewalled` > `advisory` > `direct` | where its packets go |
| `nproc`, `cpu_sec` | integer, `0` = no cap | `RLIMIT_NPROC`, `RLIMIT_CPU` |

Shipped rows, restored to these values at every start (`CONTAIN_SHIPPED`,
`src/sandbox.c`, written by `db_ensure_schema`). An operator may add rows (SQL
or the explorer); those are left alone. No agent-facing verb writes the table.

| name | files | network | nproc | cpu_sec |
|---|---|---|---|---|
| `standard` (default) | private | firewalled | 256 | 0 |
| `offline` | private | none | 64 | 120 |
| `basic-firewalled` | confined | firewalled | 256 | 0 |
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
| | `FE_LANDLOCK` | a Landlock ruleset (kernel 5.13+); for file and js the path check stays on beneath it |
| | `FE_CODE` | the path check in cclaw's own file tools and js globals |
| | `FE_NONE` | nothing (`open`) |
| network | `NE_NETNS` | an empty network namespace — the one the mount namespace brings, or a user + network namespace alone (`contain_enter_net`); the proxy socket is the only way out |
| | `NE_SECCOMP` | a seccomp filter that refuses `socket()` for every family (network `none` only) |
| | `NE_NOTIF` | a seccomp filter that hands every `connect()` to a supervisor thread in the broker, which tunnels it through the proxy; no UDP or raw sockets (the shell, no network namespace; [below](#connect-interception)) |
| | `NE_CODE` | js's curl dials the egress proxy's socket itself |
| | `NE_ENV` | the proxy runs on the host's loopback and the proxy variables are set; nothing else |
| | `NE_NONE` | nothing |

A plan with `NE_NETNS` for the shell also carries `transparent` when the host
has `NOTIF` (never under `none`): the same interception inside the namespace,
as a convenience — the namespace keeps the level either way.

### The planner

`contain_plan()` (`src/sandbox.c`) is one pure function: (row, vehicle,
`contain_caps()`) → plan or refusal. `run_tool_req_init` calls it for every
child call; `--doctor` and the startup warning call it per agent.

Strongest first, per level:

| files | file | js | shell |
|---|---|---|---|
| `private` | MOUNTNS, else refuse | MOUNTNS, else refuse | MOUNTNS, else refuse |
| `confined` | MOUNTNS, else LANDLOCK (+ path check), else CODE | MOUNTNS, else LANDLOCK (+ path check), else CODE | MOUNTNS, else LANDLOCK, else refuse |
| `open` | NONE | NONE | NONE |

| network (no mount namespace) | file | js | shell |
|---|---|---|---|
| `none` | NONE | CODE (http_request refuses) | NETNS, else SECCOMP, else refuse |
| `firewalled` | NONE | CODE | NETNS, else NOTIF, else refuse |
| `advisory` | NONE | CODE | NOTIF, else ENV (both need the backend's loopback proxy) |
| `direct` | NONE | NONE | NONE |

With the mount namespace the network enforcer is always `NETNS`: there the
proxy is the only route out, which keeps `none` and `firewalled` and keeps
`advisory` more strongly than it promises (`--doctor` says so). `direct` cannot
be combined with the mount namespace and is refused. The file vehicle opens no
socket — it is cclaw's own file code — so no network level needs an enforcer
there. js is cclaw's code too, and QuickJS exposes no socket primitive (no
`os`/`std` modules): its one way out is `http_request`.

What this means for the shipped rows:

- On a host with user namespaces, `standard`, `offline` and `basic` all run in
  the namespace sandbox, and `none` runs with no boundary.
- On a host without them but with Landlock (kernel 5.13+; Docker, Ubuntu's
  AppArmor restriction), `standard` and `offline` are refused for every
  vehicle and `basic` runs every vehicle: Landlock for files (with the path
  check under file and js), the proxy forced in code for js, and for the
  shell connect() interception (kernel 5.19+), else the proxy variables.
  `basic-firewalled` runs the shell only with interception.
- On a host with neither, `basic` runs file tools and js (path check, proxy
  forced in code) and **refuses the shell**, which needs a kernel enforcer
  for `confined`.

**Offline without namespaces.** `confined` + `none` on a host with Landlock
and seccomp is Landlock plus a filter refusing every socket: the observer
row such a host can have. It is not shipped — withholding `shell_exec`, or
`basic` with no host grants (the proxy denies everything), covers most
observer agents, and a shipped row would sit in every suggestion. An
operator who wants it adds it:

```sql
INSERT INTO containments (name, files, network, nproc, cpu_sec)
VALUES ('basic-offline', 'confined', 'none', 64, 120);
```

### The child

The plan rides the `--run-tool` wire (levels, limits, enforcers). The child
applies exactly it (`sandbox_child_setup`) and exits 126 if any step fails; it
never picks a weaker enforcer.

`contain_caps()` (`src/contain.h`) is probed once per process by fork-and-try
— `MOUNTNS` 0x1 (unshare user, mount, IPC, PID and network namespaces, map the
user, make the mount tree private: the first step that needs a capability,
which is what Ubuntu's AppArmor restriction denies), `NETNS` 0x2 (a user and
network namespace with `lo` up), `SHIM` 0x4 (the backend can serve the
loopback proxy; always on Linux), `LANDLOCK` 0x8 (a child enforces a ruleset
through `contain_landlock` and then sees `/dev/null` open and `/` refused),
`SECCOMP` 0x10 (a child installs the `socket()` filter through
`contain_seccomp` and then sees a socketpair made and `socket()` refused),
`NOTIF` 0x20 (a child installs the connect filter with
`SECCOMP_FILTER_FLAG_WAIT_KILLABLE_RECV`, and the parent takes the child's
listener with `pidfd_getfd` and reads the child's memory with
`process_vm_readv`; never offered on i386, whose programs reach the socket calls through
`socketcall` with the arguments in memory; on 32-bit ARM, where programs call
them directly, the old multiplexer is refused whole instead) —
cached in memory, never persisted. Each probe checks both directions, so a
kernel that accepts the call but enforces nothing is reported as lacking it
(`test_contain.c` runs the probe under a filter that answers as a kernel
without either, and it reports neither). `CCLAW_CONTAIN_CAPS_MASK`, a test and
diagnostic knob read only there, is ANDed in: it can only take capabilities
away (`0x1c` is a host with Landlock and seccomp and no user namespaces,
`0x3c` the same with connect() interception).
`make CONTAIN=none` reports none.

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
  under `FE_CODE` and `FE_LANDLOCK` the scratch dir itself.

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

## files `confined` without a namespace: Landlock

`FE_LANDLOCK` restricts the child, and everything it execs, to the same roots
the path check and the mount planner use (`confine_landlock`,
`contain_landlock`), applied last in child setup, after `no_new_privs` and
once the loopback shim is forked off:

| path | rights |
|---|---|
| workspace, scratch dir, `write_path` grants | everything the kernel's ABI knows (read, write, execute, create, remove, link/rename across them, truncate) |
| `read_path` grants, `/usr /bin /sbin /lib /lib64 /etc /opt` | execute, read file, read dir |
| `/dev/null /dev/zero /dev/full /dev/random /dev/urandom /dev/tty` | read, write, truncate, device ioctl |
| `/proc` | execute, read file, read dir |

The key and the DB are in no rule, so they cannot be opened, by any name a
symlink gives them; another agent's workspace, the user's home, the host's
`/tmp` and `/dev/shm` likewise. `/proc` is readable because tools need
`/proc/self`; Landlock scopes ptrace from ABI 1, so other processes'
sensitive files there (`environ`, `fd`, `mem`, `root`) are refused.

**ABI.** The child asks the kernel for its Landlock ABI and handles only the
rights that version knows: ABI 1 the base set, `REFER` from 2 (cross-directory
link and rename; without it the kernel refuses those outright), `TRUNCATE`
from 3, `IOCTL_DEV` from 5. From ABI 6 it also scopes abstract unix sockets
and signals to the child's own domain. The network rights (ABI 4) are not
used: they filter by port only. Structs, constants and the syscall numbers
(444–446 on every architecture) are local, so no new kernel headers are
needed to build.

**Also under Landlock**, there being no PID namespace: a seccomp filter
refusing `ptrace`, `process_vm_readv` and `process_vm_writev` (Landlock's
scoping is the first line; this is the belt) and io_uring.

**What it does not promise, next to the mount namespace:**

- Existence is visible: `stat`, `ls` of a parent, error codes tell a file that
  exists from one that does not. Contents are what is protected.
- No private `/tmp`, `/dev/shm` or `/proc`. A program that hard-codes `/tmp`
  gets `EACCES` (`TMPDIR` points at the scratch dir); `/dev/shm` is simply
  out of reach (so POSIX semaphores, e.g. Python multiprocessing locks, fail);
  `ps` and other processes' command lines are visible.
- `connect()` to an existing pathname socket is not mediated by Landlock:
  under `basic` the shell can reach `/run/dbus/system_bus_socket` or
  `/var/run/docker.sock`, subject to their file permissions. Network `none`
  (seccomp, no sockets at all) closes that, and so does `firewalled` under
  `NE_NOTIF`, where the supervisor makes every unix connection and refuses
  one outside the agent's directories (below).
- Below ABI 6, signals to processes of the same uid outside the child (the
  daemon included) and abstract unix sockets are reachable.
- A hard link to the key placed in the workspace by someone outside the child
  is readable (the mount namespace masks by path too). The child cannot make
  one: a link needs the source's directory in a rule.

For file and js, the path check runs first and Landlock closes its
symlink-swap race. One difference is visible: the path check allows the
system directories, `/dev` included, so under `FE_CODE` a file in the host's
`/dev/shm` is readable; Landlock refuses it.

## files `confined` without a namespace or Landlock: the path check

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
could pass it, which is why Landlock is added beneath it when the kernel has
it (`FE_LANDLOCK`).

## network without the mount namespace

**`NE_NETNS` alone** (files `open`, or `confined` by Landlock, on a host with
user namespaces): `contain_enter_net` unshares a user and a network namespace,
maps the user to itself and brings `lo` up; the files stay the host's. The
shim reaches the proxy socket at its real path in the agent folder, and the
ssh connect helper is written to the scratch dir. A program that ignores the
proxy variables has no route anywhere — the host's own loopback included. No
PID namespace (it would need its own `/proc`, which needs the mount namespace).

**`NE_SECCOMP`** (the shell under network `none` with no network namespace):
`socket()` fails with `EACCES` for every family — inet, inet6, netlink and
unix, so a pathname socket on the host (`docker.sock`) is closed too —
`socketpair()` still works (it reaches nothing), io_uring is `ENOSYS` (its
operations would bypass a syscall filter), and on i386 the `socketcall`
multiplexer's socket call is refused the same way (on 32-bit ARM the whole
multiplexer, which EABI programs do not use). The filter's first
instruction kills any architecture but the build's (and, on x86_64, the x32
ABI).

**`NE_ENV`** (`advisory` on a host without interception): the shell needs
`AF_INET` to reach the loopback shim, so there is no socket filter:
`docker.sock` and other pathname sockets remain reachable subject to file
permissions, and so does anything a program can dial while ignoring the proxy
variables. That is what advisory says.

## connect() interception

`src/contain_linux_notif.c`. The shell child's seccomp filter returns
`SECCOMP_RET_USER_NOTIF` for `connect()` and the child sends the listener (and
the DNS stub's socket, and the shim's port) to the broker over a socketpair
made before the fork. A thread in the broker — outside every namespace,
Landlock domain and filter, an ancestor of every contained process — answers
each call on a thread of its own (a slow destination holds up only its own
caller; every wait ends when the caller stops waiting, checked each second):

1. `pidfd_open`, `SECCOMP_IOCTL_NOTIF_ID_VALID`, `pidfd_getfd` takes the
   socket object the call names, `process_vm_readv` reads the address once,
   `NOTIF_ID_VALID` again. Any failure: `EPERM`.
2. An inet or inet6 stream socket to anywhere but loopback: the supervisor
   connects that socket itself to the shim's loopback port, sends the shim's
   `CONNECT host:port`, reads the reply's head a byte at a time (what the
   destination sends first stays for the caller) and returns 0 — the caller
   holds a real TCP socket, tunnelled. A refusal disconnects it
   (`AF_UNSPEC`) and returns `ECONNREFUSED`; the proxy's denial note reaches
   the tool result as for any other connection. `host` is the name the DNS
   stub handed the address out for, or the literal address.
3. Anything else: continued inside a network namespace (the kernel already
   holds the line there); performed by the supervisor on the object it took
   when interception is the enforcer, never continued — the kernel would
   re-read an address the caller can change after the check.

The flag `WAIT_KILLABLE_RECV` (kernel 5.19) is required: without it a Go
runtime's preemption signals restart the call under the supervisor.
`getpeername()` on a tunnelled socket shows the shim, not the destination.
Docker's default seccomp profile allows `pidfd_getfd` only with
`CAP_SYS_PTRACE` (`--cap-add SYS_PTRACE`), so there the probe reports no
`NOTIF`.

**`NE_NOTIF`** (the shell, no network namespace: `firewalled`, and `advisory`
when the host has it). The filter also decides, on register arguments only:
`socket()` for inet and inet6 only as a TCP stream (UDP, raw, SCTP, MPTCP:
`EACCES`), unix and netlink as asked, every other family `EACCES`;
`sendto`/`sendmsg`/`sendmmsg` with `MSG_FASTOPEN` `EPERM` (it connects
without `connect()`); ptrace and `process_vm_*` `EPERM` (a traced process
outside the filter would connect for the caller); io_uring `ENOSYS`. The
supervisor connects loopback destinations and netlink directly, and unix
pathname sockets with a relative path resolved against the caller's working
directory; an abstract unix address only under files `open` (Landlock's
scoping would refuse it). So: no UDP and no DNS — a program that ignores the
proxy reaches only literal addresses the proxy allows.

What is left of the host itself depends on the level. Under `advisory` the
host's loopback stays reachable (the agent's own dev servers, and whatever
else listens there), any pathname socket the user may open connects, and
listening sockets are untouched. Under `firewalled` the host's own services
are out of reach as well, since a local daemon would act or connect for the
caller:

- on loopback only the shim's port connects (`ECONNREFUSED` otherwise), so
  the agent cannot reach a server it started itself either;
- a unix socket connects only when its file — the real path, after links —
  is under the workspace, the scratch dir or a path grant, or is the proxy's
  own socket (`EACCES` otherwise). The supervisor holds the file open and
  connects to what it holds, so a link swapped in afterwards is not followed.
  Under files `open` every pathname socket connects: that level already lets
  the shell write anywhere the user can;
- `listen()` is `EACCES`, for unix sockets too: nothing connects in.

A datagram sent to a unix address without `connect()` (`sendto` to
`/dev/log`) is not seen by the supervisor and still goes.

**Transparent** (`NE_NETNS` + `NOTIF`): unhandled connects are continued.
With the mount namespace the child also binds a UDP socket on
`127.0.0.1:53` before the exec and `/etc/resolv.conf` says
`nameserver 127.0.0.1` (a bind of our own file, or, when it is a symlink,
our file created where it points on the root tmpfs). The stub answers an A
query with one address per name from `198.18.0.0/15` for the life of the
call, AAAA with an empty answer, anything else `NOTIMP`; a name with other
than hostname characters is `NXDOMAIN`. No lookup happens there: the proxy
resolves the real name when the connection is made. A network namespace
without a mount namespace has no resolv.conf to replace: interception
without the stub.

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
`test/test_contain_code.c` (with capabilities masked, through the real
`--run-tool` child: `basic` by the path check and by Landlock for every
vehicle, the shell under `confined` + `none` with no sockets, a network
namespace without a mount namespace, js through the proxy, refusals;
interception: a proxy-unaware program by name in the namespace,
`basic-firewalled` without namespaces in both directions and the host's
own services closed to it (open to `basic`), a sockaddr
rewritten during the call, the DNS stub's malformed packets),
`test/test_contain.c` (the probe in both directions, the mask), the namespace
suites (`test_shell_namespace.c`,
`test_run_tool_escape.c`, `test_sandbox_mounts.c`, `test_sandbox_key_mask.c`,
`test_integration_shell_proxy*.c`), and the per-column cap in
`test/test_agent_define.c`.

## Not built

- Landlock's TCP port rules (ABI 4): port-only, a speed bump, not a boundary.
- js through a network namespace without the mount namespace: js keeps
  `NE_CODE` there, which needs no namespace and keeps the same promise.
- Dropping to another uid under a root daemon; cgroups.
