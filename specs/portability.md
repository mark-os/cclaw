# Portability — layers, and what each needs from the host

Status: **direction, partly built.** The goal and the layer model are settled.
The "as built" column says what the code does today; the gaps are listed at
the end as ordered work. Nothing here changes behaviour by itself.

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

Layer 2 is the bare-minimum *useful* offering: an agent that can read and
write its workspace, fetch allowed hosts, run and author JavaScript tools, and
reconfigure itself — with no shell.

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
| Host allowlist, `web_fetch` and JS `http_fetch` | Network namespace: the only route out is `HTTP_PROXY` → `net_shim` → the broker's proxy → `host_decide()` | The decision is; the *forcing* is the namespace |
| Host allowlist, shell | Network namespace + preload shim → the same proxy | Same |

So on a kernel without unprivileged user namespaces there are two outcomes
and nothing between them:

- `standard` / `restricted`: every tool call exits 126 (fail-closed,
  [sandbox-profiles.md](sandbox-profiles.md)). The agent can talk and
  reconfigure itself, and nothing else.
- `host`: every tier runs with no policy at all — any file, any host, and the
  shell.

`--doctor` reports this as one line under `[userns]`.

What is already in good shape:

- **The Linux-only seam is narrow.** Namespace, mount and `prctl` calls live
  in `sandbox.c`, `run_tool.c`, `channel.c` and the `--doctor` probe.
- **Old kernels.** Outside `sandbox.c` the only post-3.x syscalls are
  `getrandom` and `close_range`, and both fall back.
- **One egress decision.** `host_decide()` in `proxy.c` is the single
  implementation, with resolve-then-dial-literal against DNS rebinding
  ([egress-filter.md](egress-filter.md)). Mediation must reuse it, not copy it.

## Work, in order

1. **Path policy in code.** One function answers "may this call read / write
   this path": canonicalise (symlinks, `..`), then workspace, CWD mount,
   read/write grants, and an unconditional refusal of the DB and key files.
   File tools and JS `fs.*` call it before every open.
2. **Egress without a network namespace.** The web and JS tiers keep using the
   broker's proxy; what changes is how their libcurl reaches it when there is
   no namespace to run `net_shim` in. The decision function is not duplicated.
3. **A mediated mode.** Layers 1–2 run without a namespace, enforced by (1)
   and (2); the shell is refused. This replaces "everything exits 126" as the
   answer for a host with no containment backend. See open decisions.
4. **Containment as a backend.** `sandbox.c` behind a small interface
   (`linux-ns`, `none`), so a platform with its own sandbox supplies its own.
5. **`--doctor` reports layers.** For each layer: in force or not, enforced by
   what, and what the host is missing.

## Open decisions

- **How mediated mode is chosen.** Recommended: an explicit `sandbox_profile`
  value, so a transient `unshare` failure can never silently weaken a
  sandboxed agent, with `--doctor` telling the operator to set it when the
  probe fails. The alternative is choosing it automatically at startup.
- **Whether the path policy also applies inside the namespace.** Applying it
  everywhere means one code path that is always exercised, but file tools
  could then no longer read the read-only system directories the sandbox
  mounts (`/usr`, `/etc`). Applying it only in mediated mode keeps today's
  behaviour and leaves the new check exercised only by tests and old devices.
- **The preload shim on musl hosts** ([static-build.md](static-build.md)) —
  a layer 4 question, unaffected by the above.
