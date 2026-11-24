# Portability — layers, and what each needs from the host

Status: **direction.** The goal and the layer model are how the code should
be organised; "As built" says what it does today. Nothing here changes
behaviour by itself.

## Goal

Run on the widest range of old, embedded and $5 Linux devices, and later on
platforms that bring their own sandbox (Android first). Two consequences:

- **The floor is low and explicit.** The least a device must offer is a
  kernel, a filesystem and a route to an LLM endpoint. On that floor cclaw is
  still an agent: the loop, SQLite, self-reconfiguration, and JavaScript.
- **Capability is added in layers, never assumed.** A device that lacks what
  a layer needs loses that layer and nothing below it. `cclaw --doctor` says
  which layers are in force and what the missing one needs.

Two ways to get a binary, both supported on purpose:

- **Release binary** — static musl with curl and TLS inside
  ([static-build.md](static-build.md)). No libc or libcurl floor; the kernel is
  the only "how old" number left. This is the portability vehicle.
- **Source build** — `make` against the host libc and system libcurl, for
  developers and for targets we do not build for.

## The layers

| # | Layer | What it is | Needs from the host |
|---|---|---|---|
| 1 | **Core** | Agent loop, SQLite, config and self-reconfiguration, LLM requests | Kernel, filesystem, a CA bundle for HTTPS |
| 2 | **Mediated tools** | File tools, JavaScript (`js_eval`, extension tools such as `web_fetch`) | Nothing: cclaw's own code is the only thing acting, so cclaw's own code can enforce the policy |
| 3 | **Containment** | A boundary imposed from outside on code cclaw cannot inspect | A backend: Linux namespaces today; a platform sandbox (Android app sandbox) or none elsewhere |
| 4 | **Shell** | Arbitrary programs (`shell_exec`) | Layer 3, or the operator's explicit opt-out (containment `none`) |
| 5 | **Daemon** | Channels, cron, webhooks, multi-agent | `fork`/`exec` of the cclaw binary itself, signals |

Layers 1 and 2 are the bare-minimum useful offering: an agent that can read
and write files, fetch from the network, run and author JavaScript tools, and
reconfigure itself — whether or not it has a shell.

Layers 2 and 4 differ in how policy can be enforced. **Mediation** is
enforcement inside cclaw, possible wherever cclaw's code is the actor.
**Containment** (the axis of that name in [trust.md](trust.md)) is enforcement
from outside, required wherever it is not. The shell needs containment because any
command can do anything. File access, HTTP and QuickJS do not, because every
file open and every connection is a line of cclaw's C.

## As built (2026-11-24, step 4a)

Each level of an agent's containment is kept by the strongest enforcer the
host has ([containment.md](containment.md)); what no enforcer can keep is
refused at dispatch. Layer 2 now exists for files `confined`:

| Policy | With user namespaces | Without them |
|---|---|---|
| Paths, file tools and JS `fs.*` | Mount namespace: an ungranted path is absent | `confined`: the path check in cclaw's code (`contain_path_check`); `private`: refused |
| `cclaw.db` / `.cclaw_key` | Outside every mount set, masked if a bind would show them | The path check refuses them by path and by inode |
| Host allowlist, `web_fetch` and JS `http_request` | Network namespace; the only route out is the broker's proxy | curl dials the proxy's unix socket itself (`NE_CODE`): the same `proxy_decide()`, metadata and private-address blocks, SNI check, narrowing and contacted-host log |
| Host allowlist, shell | Network namespace + `net_shim` | `advisory`: the shim on the host's loopback and the proxy variables, nothing more; `proxied`/`none`: refused |
| Files, shell | Mount namespace | `confined`: refused until Landlock (4b) |

So on a kernel without unprivileged user namespaces:

- `standard` / `offline`: every tool call is refused at dispatch, with the
  reason and a containment the host can keep.
- `basic`: file tools and JS (so `web_fetch`) run with paths checked and
  egress forced through the proxy in code; the shell is refused.
- `none`: every tool runs as the user, with no policy.

`--doctor` `[layers]` prints, per agent, each vehicle's enforcers or refusal.

What is already in good shape:

- **The containment mechanism is one file.** `src/contain.h` is the backend
  interface (name, capabilities, hint, enter, egress); `contain_linux.c`
  implements it with namespaces and `contain_none.c` with nothing (no
  capabilities: `basic`'s file and js tools and `none` run). `sandbox.c` holds
  the planner, the path check and child setup. What remains Linux-specific
  outside it is `prctl(PR_SET_PDEATHSIG)` in `run_tool.c` and `channel.c`, and
  reading `/proc/self/exe`.
- **Old kernels.** Outside `contain_linux.c` the only post-3.x syscalls are
  `getrandom` and `close_range`, and both fall back.
- **One egress decision.** `proxy_decide()` in `proxy.c` is the single
  implementation ([egress-filter.md](egress-filter.md)); mediation reuses it
  over the same socket.

## Boundaries, by what the host offers

- **Host with user namespaces**: everything in [security.md](security.md) and
  [containment.md](containment.md) holds; `standard`, `offline` and `basic`
  run every vehicle in a namespaced child.
- **Host without them** (old or vendor kernels, Docker's default profile,
  Ubuntu's AppArmor restriction, Android): `basic` keeps file and JS policy in
  cclaw's code, and `none` means what it says — the agent acts as the user
  running cclaw, on any file and any host, and agents are not isolated from
  each other. The device is the boundary.

A containment never slides from one to the other: a level the host cannot
keep is refused, not weakened.

## Work, in order

Done: the capability probe (`contain_caps`), `--doctor` `[layers]`,
containment as a backend (`CONTAIN=linux|none`), the startup warning naming a
containment the host can keep (with the backend's hint — on Linux whichever of
the AppArmor restriction, `unprivileged_userns_clone` or `max_user_namespaces`
is set against it), and mediation without a namespace (`basic`).

1. **Kernel enforcers without a namespace (4b).** Landlock for files
   `confined` (the shell, and a backstop under the path check), seccomp for
   network `none`, a network namespace without a mount namespace.
2. **Platform backends.** A build for a platform with its own sandbox
   (Android) starts from `contain_none.c`.

## Decided

- **No code is loaded into the host's programs.** The `LD_PRELOAD` shim that
  made the sandboxed shell's network transparent to proxy-unaware programs
  was the last host-libc dependency, and was removed in v0.6.1: layer 4 now
  needs layer 3 and nothing else from the host. The cost is that raw-socket
  programs have no network inside the sandbox; ssh goes through `net_shim`'s
  `connect` mode ([shell-networking.md](shell-networking.md)).
