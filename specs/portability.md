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
| 1 | **Core** | Agent loop, SQLite, config and self-reconfiguration, LLM requests, hooks | Kernel, filesystem, a CA bundle for HTTPS |
| 2 | **Mediated tools** | File tools, `web_fetch`, JavaScript (`js_eval`, extension tools) | Nothing: cclaw's own code is the only thing acting, so cclaw's own code can enforce the policy |
| 3 | **Containment** | A boundary imposed from outside on code cclaw cannot inspect | A backend: Linux namespaces today; a platform sandbox (Android app sandbox) or none elsewhere |
| 4 | **Shell** | Arbitrary programs (`shell_exec`) | Layer 3, or the operator's explicit `host` opt-out |
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

## As built (2026-11-12)

Layer 2 does not exist yet as a separate thing. All four tool tiers run in a
`cclaw --run-tool` child that enters the same namespace sandbox, and the
policy for the "mediated" tiers is in fact enforced by that namespace:

| Policy | Where it is enforced today | In cclaw's code? |
|---|---|---|
| Workspace and path grants, file tools | Mount namespace: an ungranted path is simply absent. `tool_file.c` opens whatever path it is given; `path_grant_hint` only words the error | No |
| Workspace and path grants, JS `fs.*` | Mount namespace. `fs.readFile` is a bare `fopen` | No |
| `cclaw.db` / `.cclaw_key` masking | Mount namespace (outside every mount set) | No |
| Host allowlist, `web_fetch` and JS `http_request` | Network namespace: the only route out is `HTTP_PROXY` → `net_shim` → the broker's proxy → `host_decide()` | The decision is; the *forcing* is the namespace |
| Host allowlist, shell | Network namespace: the same `HTTP_PROXY` → `net_shim` → proxy path, for programs that honor the variables; the rest have no network | Same |

So on a kernel without unprivileged user namespaces there are two outcomes
and nothing between them:

- `standard` / `restricted`: every tool call exits 126 (fail-closed,
  [sandbox-profiles.md](sandbox-profiles.md)). The agent can talk and
  reconfigure itself, and nothing else.
- `host`: every tier runs with no policy at all — any file, any host, and the
  shell.

`--doctor` `[layers]` reports which of these applies, per agent.

What is already in good shape:

- **The containment mechanism is one file.** `src/contain.h` is the backend
  interface (name, probe, enter, egress); `contain_linux.c` implements it with
  namespaces and `contain_none.c` with nothing. `sandbox.c` holds only policy
  and child setup. `make CONTAIN=none` builds without the Linux backend. What
  remains Linux-specific outside it is `prctl(PR_SET_PDEATHSIG)` in
  `run_tool.c` and `channel.c`, and reading `/proc/self/exe`.
- **Old kernels.** Outside `contain_linux.c` the only post-3.x syscalls are
  `getrandom` and `close_range`, and both fall back.
- **One egress decision.** `proxy_decide()` in `proxy.c` is the single
  implementation — allowlist, one resolution, IP filter, and the literal
  address to dial ([egress-filter.md](egress-filter.md)). It is a plain
  function over the rule set. Mediation must reuse it, not copy it.

## Boundaries, by what the host offers

- **Host with a containment backend** (Linux with unprivileged user
  namespaces): everything in [security.md](security.md) and
  [sandbox-profiles.md](sandbox-profiles.md) holds. Every tool call, JS
  included, runs in a forked, namespaced child.
- **Host without one** (old kernels, vendor kernels built without user
  namespaces, Android): the `host` profile is the answer, and it means what it
  says. The agent acts as the user running cclaw: any file that user can read,
  including cclaw's own database and key; any host on the network; the shell.
  Agents are not isolated from each other, so a sub-agent has the same reach
  as its parent. The device is the boundary, and the network boundary is the
  operator's firewall.

That second case is a supported way to run cclaw, not a degraded one: a
lightweight agent loop on a device given over to it. What it must never be is
a surprise — a sandboxed profile still fails closed rather than sliding into
it.

## Work, in order

Done: one probe for "can this host contain a child" (`contain_probe`),
`--doctor` `[layers]`, and containment as a backend (`CONTAIN=linux|none`).

1. **Make "no containment here" a first-class answer at first run.** A fresh
   install on such a host still gets the `standard` profile and every tool
   call exits 126; `--doctor` now explains it, first run does not.
2. **Platform backends.** A build for a platform with its own sandbox
   (Android) starts from `contain_none.c`.

## Placeholders — layer 2 enforced without a namespace

Not planned work. Recorded so the code leaves room for it.

- **Egress for `web_fetch` and JS.** The proxy is ordinary socket code and
  needs no namespace; only *forcing* traffic through it does. cclaw's own
  libcurl can simply be pointed at it, which would give these two tiers a real
  host allowlist on any host. The shell cannot be forced this way: proxy
  environment variables are advice a program is free to ignore.
- **Path policy in code.** One function deciding "may this call read / write
  this path" for file tools and JS `fs.*`, in place of relying on what is
  mounted.

## Decided

- **No code is loaded into the host's programs.** The `LD_PRELOAD` shim that
  made the sandboxed shell's network transparent to proxy-unaware programs
  was the last host-libc dependency, and was removed in v0.6.1: layer 4 now
  needs layer 3 and nothing else from the host. The cost is that raw-socket
  programs have no network inside the sandbox; ssh goes through `net_shim`'s
  `connect` mode ([shell-networking.md](shell-networking.md)).
