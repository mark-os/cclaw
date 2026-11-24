# Tool Execution Model — Unified `--run-tool` Entry, Vehicle Dispatch, Shared Network Broker

Status: **as built.** The structure every sandboxed tool child (the file,
shell and js vehicles) runs on: one re-exec'd `--run-tool` entry point, a
vehicle descriptor, and one shared helper for the two that reach the network. The boundary itself is the
containment backend ([portability.md](portability.md), layer 3;
[containment.md](containment.md)); this note covers the processes
around it. All phases in §9 are done.

---

## 1. Why every child execs first (rationale)

The daemon is multithreaded: poll-loop thread + elastic LLM worker pool (each
worker owns its own `sqlite3*` + `CURL*`, runs blocking `llm_req`) + civetweb
threads. Any of these may hold a malloc-arena / sqlite / curl / stdio lock at
any instant.

A fork-only child of a multithreaded process has one thread but inherits
**all locks in whatever state they were at the fork instant** — including
locks held by *other* daemon threads, now frozen with no owner. Its first
`malloc` (in `proxy_bind`, a parse, etc.) can deadlock. "Single-threaded"
(thread count) ≠ "clean lock state". The shell broker was once such a
fork-only child.

`execve` is the only operation that discards inherited locks — it throws away
the whole address space and builds a fresh one. Therefore **every process that
runs real (allocating) logic after forking from the daemon must exec first.**
Every child vehicle now does.

---

## 2. Minimal-exec analysis (drives the whole structure)

Count execs forced by a real boundary — clean lock state, or a different
program:

| Vehicle | exec #1 (sterilize) | inner fork | inner exec (foreign program) | Total |
|---------|---------------------|------------|------------------------------|-------|
| file    | yes → `--run-tool`  | none       | none                         | **1** |
| js      | yes → `--run-tool`  | yes (fork) | none — child runs OUR qjs    | **1** |
| shell   | yes → `--run-tool`  | yes (fork) | yes — `execl /bin/sh`        | **2** |

Key consequences:

- **One exec from the daemon, into one entry point (`--run-tool`), for every
  child vehicle.** That single exec sterilizes inherited locks. There is **no
  separate `--broker` or `--web-fetch` mode** — the "broker" is a *role the
  `--run-tool` process plays for shell and js*, a code branch, NOT a process image. Becoming
  the broker costs **zero** extra execs.
- **js sandbox children are plain `fork`s, not fork+exec.** They run
  first-party C (QuickJS, whose `http_request` is our curl) and fork from the already-sterile `--run-tool` process
  (single-threaded, clean locks), so the child fork is the safe
  single-threaded-fork case. No exec needed to run our own functions.
- **shell is the ONLY vehicle with a second exec**, and only because `/bin/sh` is a
  foreign program. That exec is irreducible and lives in exactly one place.
  The interpreter is configurable (`agents.shell_path` / `CCLAW_SHELL_PATH`,
  see docs/configs.md) but defaults to `/bin/sh`; either way it's still one
  foreign-program exec.

---

## 3. Control flow

```
daemon poll loop
  └─ fork + execve  cclaw --run-tool   ← THE one mandatory exec (sterilize locks)
       run_tool_main():
         run_tool_verify_clean()                ← shared: no key, fd-set=={0,1,2,3}, no DB
         read + parse request blob from fd 3
         desc = vehicle_descriptor(blob.vehicle) ← {skip_pid_ns, needs_proxy, inner_exec, run_fn}
         if !desc.needs_proxy:               // FILE
             setup_ns(desc.skip_pid_ns)      // no_net, scoped mount set
             result = desc.run_fn(blob)      // in-process, this image
             write result → fd 3; _exit
         else:                               // JS, SHELL
             serve_network_child(desc, blob) // shared helper, see §4
             write result → fd 3; _exit
  └─ daemon tracks ONE pid (the --run-tool process) via g_children[],
     drains its result over fd 3, reaps via existing sigchld path.
```

`--run-tool` intercept is at the **pre-init position** (top of `main`, before any
DB/config/key init — NOT the `--channel` site, which runs after init). This is
what makes "no key, no DB" true by construction; `run_tool_verify_clean` then proves it.

---

## 4. The shared network helper (the de-dup)

The two network vehicles (js, shell) differ on only a few small axes;
everything else is shared in one helper (`serve_network_child`):

```
serve_network_child(const VehicleDescriptor *desc, RunToolParsed *q,
                    unsigned char *body, size_t body_len):
    proxy_bind(&proxy, agent_dir, q->host_rules, q->deny_rules)
        // bind while single-threaded, BEFORE the fork below;
        // skipped under containment none (no sandbox) and offline (no network)
    pid = fork()                         // safe: this image is sterile/1-thread
    if child:
        setpgid(0,0)
        wire stdout/stderr → pipe
        setup_ns(desc.skip_pid_ns, ...)  // fail-closed; _exit on sandbox failure
        if desc.inner_exec:
            tool_shell_child_exec(q)      // execs /bin/sh, never returns
        else:
            desc.run_fn(q)               // ← in-process leaf (js)
        _exit(...)
    // parent (the --run-tool/broker process):
    proxy_serve(&proxy)                  // start accept thread AFTER the fork
    drain child stdout with timeout      // shell's existing drain loop, shared
    waitpid(child); kill(-pgrp) on timeout
    proxy_stop(&proxy)
    return result
```

Per-vehicle inputs to the helper:

| Axis        | file            | js                           | shell                        |
|-------------|-----------------|------------------------------|------------------------------|
| skip_pid_ns | 1               | 1                            | 0 (PID ns)                   |
| needs_proxy | no              | yes                          | yes                          |
| inner_exec  | 0               | 0                            | 1                            |
| run_fn      | tool_file_child_run (not via helper) | tool_js_child_run (qjs eval, **in-process**) | NULL (`execl /bin/sh`, **inner exec**) |
| /proc       | absent          | absent                       | present (PID ns backs it)    |
| proxy reach | n/a             | net_shim + HTTP_PROXY (qjs http_request uses curl) | net_shim + HTTP_PROXY; `connect` mode for ssh (untrusted subprocess tree) |

`web_fetch` is not a vehicle: it is the shipped `web-fetch` extension, a JS
tool (`extensions/web-fetch/fetch.qjs`) whose `http_request` and
`HTML.toMarkdown` run in the js child like any extension tool's.

Invariant restated: **/proc present IFF PID namespace present.** Every
network vehicle gets the proxy socket bind-mounted at one fixed in-boundary path,
plus `net_shim` listening on loopback; the proxy variables point at the shim.
Only the shell, which runs foreign subprocess trees, also gets the
`connect` helper for ssh.

Adding a future network vehicle = a new `(skip_pid_ns, run_fn)` pair plugged into
the table. No new process mode, no new exec, no new drain/proxy code.

---

## 5. Vehicle descriptor (single source of truth)

A tool's recipe names one `ExecVehicle` (`src/tools.h`): `EXEC_INLINE`,
`EXEC_THREAD`, or one of the three child vehicles `EXEC_FILE`, `EXEC_SHELL`,
`EXEC_JS` (`vehicle_is_child()`). The same value is the first byte of the
request blob. Decode `blob.vehicle` ONCE in `run_tool_main` into a small
struct; pass it down. Do not scatter `if vehicle == EXEC_SHELL` across call
sites.

```c
typedef char *(*RunFn)(const RunToolParsed *q);

typedef struct {
    int   skip_pid_ns;   /* 1 = no CLONE_NEWPID (file/js); 0 = PID ns (shell) */
    int   needs_proxy;   /* 1 = bind/serve proxy + inner fork (shell/js) */
    int   inner_exec;    /* 1 = run_fn execs a foreign program (shell) */
    RunFn run_fn;        /* leaf result producer; NULL iff inner_exec */
} VehicleDescriptor;
```

`vehicle_descriptor(vehicle)` is a compile-time table mapping the three child
vehicles to their axes; any other byte (an in-process vehicle, or garbage) is
refused. "What makes js different from shell" lives in this one table:

```c
static const VehicleDescriptor table[] = {
    [EXEC_FILE]  = { .skip_pid_ns = 1, .needs_proxy = 0, .inner_exec = 0, .run_fn = tool_file_child_run },
    [EXEC_SHELL] = { .skip_pid_ns = 0, .needs_proxy = 1, .inner_exec = 1, .run_fn = NULL },
    [EXEC_JS]    = { .skip_pid_ns = 1, .needs_proxy = 1, .inner_exec = 0, .run_fn = tool_js_child_run },
};
```

---

## 6. Request blob (fd 3)

4-byte LE body-length prefix + a flat little-endian binary body, cap
`RUNTOOL_REQUEST_MAX` (32 KB), fail-closed over cap on the serialize path
(`run_tool_serialize_request` returns NULL when `w_finalize` finds the body
exceeds the cap). **Not JSON**: both ends are the same binary, so the format is
a fixed-order sequence of primitives — `u32` (4-byte LE) and `str` (`u32`
length + raw bytes, length 0 = NULL). No escaping, no key scan; secret values
cross as raw bytes with no escape/unescape round-trip. The body opens with a
vehicle byte (the `ExecVehicle` value of `EXEC_FILE`, `EXEC_SHELL` or
`EXEC_JS`); `run_tool.c` is the canonical field order. Fields by vehicle
(additive over file):

- all: `vehicle`, `tool_name`, `env_mode`, `rlimits{nproc,as_mb,cpu_sec}` (`as_mb` is
  carried but no containment sets it — see [containment.md](containment.md))
- file: `workspace`, `read_paths[]`, `write_paths[]`, `mount_cwd`, `cwd_path`, `tmp_dir`, `db_path`
  (`db_path` is a *path only* — the child never opens the DB. It is carried so
  the child can bind-mask `.cclaw_key` and the DB family out of any granted
  mount; see [security.md](security.md). Dropping it silently disables the
  mask, which is why `test/test_sandbox_key_mask.c` guards it.)
- shell/js: `host_rules[]` (exact/suffix), `deny_rules[]` (sensitivity labels), `agent_dir` (for the proxy socket; the broker partitions grants into host/CIDR rules at `proxy_bind`)
- shell: `command` (with secrets already interpolated by the daemon parent), `secrets[]` (minimal name/value set for env injection)
- shell/js: `timeout` — the call's budget in seconds. Each tool takes a
  `timeout` argument (defaults 30 shell — config `shell_timeout` — / 120 js,
  which a JS tool may undercut for its own work: web_fetch gives its fetch
  60), clamped by the
  parent to `TOOL_TIMEOUT_MAX_SEC` (600); the daemon's own backstop deadline is
  set margin-seconds past it so the broker's teardown wins in the normal case
- file/js: `params[]` — the tool's arguments, pre-extracted by the parent
  (`tool_args_extract`, SQLite JSON1 over the tool's schema) as
  `(key, kind, value)` where kind is text, opaque-JSON (js_eval's `args`,
  which QuickJS parses natively), or string-list (file_edit's edits,
  flattened to oldText/newText pairs). The raw arguments JSON never crosses
  the boundary and the child parses no JSON.

**Arguments**: validated (`json_valid`, object) at the dispatch gate and
decomposed once in the trusted parent; malformed arguments become an error
tool-result and never dispatch. Per-argument policy (`policy_eval`) matches on
the same SQLite parse — an unparseable policy blocks the call
(`POLICY_ERROR`), never fails open.

**Secrets**: resolved by the **daemon parent** before the blob is written. The
sterile `--run-tool`/broker holds no key and never interpolates: parent
interpolates → blob carries resolved values → child consumes. The broker
`explicit_bzero`s its copy of the command, the secrets and the blob right after
the fork, and the parent wipes its own.

### Response frame (fd 3, child → parent)

`[1-byte status][4-byte meta_len (network order)][hosts JSON][result to EOF]`.

`status` is `RUNTOOL_STATUS_OK`/`_ERROR` — the child's own verdict on the call,
travelling with the body so the parent never reads the result text to decide
whether the tool failed (see
[error-handling.md](error-handling.md#tool-failure-is-an-explicit-status)). It
is *not* the exit code: exit status stays lifecycle-only. `meta_len` is 0 for
the file vehicle and every error path — an unframed write would be misparsed
by the parent's drain loop, which reassembles this header across reads.

---

## 7. Sterility gates (shared, every child)

`run_tool_main` enforces sterility at process entry, before reading the request.
Fail-closed (`_exit` on violation), production gate not debug-only:

1. **FD-SET**: every fd above `{0,1,2,3}` is blanket-`close()`d (4..`OPEN_MAX`)
   immediately on entry — this *establishes* the invariant rather than auditing
   it, so a daemon fd that leaked past O_CLOEXEC is closed, not merely detected.
2. **KEY-ABSENT**: `run_tool_verify_clean()` asserts `db_secret_key_loaded() == 0`.
3. **NO-DB**: no DB handle open (true by construction at the pre-init intercept;
   the key-absent check is its observable proxy).

The broker (shell and js) is subject to the same gates — it too holds no key
and no DB. Only the proxy config + request blob cross fd 3.

---

## 8. What the broker move left unchanged

- Deny-by-default egress; the `proxy_decide()` pipeline (host rules → resolve →
  IP/CIDR floor → metadata carve-out, enforced for a numeric target and for
  every address a hostname resolves to).
- `proxy_bind` / `proxy_serve` split (bind single-threaded, serve after the
  fork), resolve-then-dial-literal anti-TOCTOU/rebind. Reused
  verbatim — they move INTO the exec'd broker, their logic is untouched.
- net_shim as the dumb forwarder for CONNECT tunnels and plain-HTTP requests
  (structurally no resolver → per-hop hostname gating is real). Every network
  child reaches it the same way: proxy variables; the shell also gets
  `connect` mode for ssh.
- Crash-only discipline: the broker is a per-call disposable PROCESS — SIGKILL
  on timeout reclaims proxy threads, relays, fds, memory. (A thread could not
  provide this — isolation requires a process.)

---

## 9. Phasing

1. ✅ **Move shell onto the exec'd `--run-tool` path** (2026-06). Shell
   `fork_tool_exec` now forks+execs `cclaw --run-tool`; the re-exec'd broker
   is single-threaded with clean locks, runs `proxy_bind`/`proxy_serve`, forks
   the sandbox child, drains output, and waits. Fixes the fork-only-broker
   deadlock hazard. Daemon-side secret interpolation + `explicit_bzero` on blob
   replaces the post-fork bzero dance. The `serve_network_child` consolidation
   (§4) is deferred to Phase 2 so file and shell share the helper instead of
   separate code paths.
2. ✅ **Add web as a second `(skip_pid_ns, run_fn)` pair** on the same helper
   (2026-06). A curl + html_to_markdown leaf ran in-process in the inner fork
   (no inner exec). `serve_network_child` is the shared path for every
   network child; shell's pre-flight `http_check_policy` is subsumed by
   per-hop `decide()`.
3. ✅ **Add js as the third network pair** (2026-06).
   `tool_js_child_run` (QuickJS eval, `http_request` via curl + HTTP_PROXY) on
   the same `serve_network_child` path. Dispatch in `src/dispatch.c` for both
   `js_eval` and JS-defined extension tools routes through the js vehicle.
4. The file tools were already on the exec'd path; they share
   `run_tool_verify_clean` and the descriptor dispatch (no separate code path).
5. ✅ **Fold web into js** (2026-11). `web_fetch` became the shipped
   `web-fetch` extension; `html_to_markdown` became the `HTML.toMarkdown`
   global, and `http_request` gained what web_fetch needed (`content_type`,
   `truncated`, the denied-host hint). The web child, its dispatch branch and
   its wire byte are gone.

---

## 10. Open items

- shell `kill(-pgrp)` across the PID-ns boundary on timeout — verify it reaches
  the inner tree.
- Global egress floor (Q2 from egress-filter.md) + PSL-aware suffix matching
  (Q1) remain deferred; the broker reads whatever rules cross fd 3.
