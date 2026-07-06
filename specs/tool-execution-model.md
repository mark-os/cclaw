# Tool Execution Model — Unified `--run-tool` Entry, Tier Dispatch, Shared Network Broker

Status: spec. Target architecture for the sandboxed tool tiers (file, web,
shell). Supersedes the current split where `tool_shell_handler` runs in a
**fork-only** (no-exec) broker child of the multithreaded daemon.

This note pins the shared structure both network tiers sit on, so the
shell-broker extraction and the web tier land against one frozen target.

---

## 1. The problem this fixes

The daemon is multithreaded: poll-loop thread + elastic LLM worker pool (each
worker owns its own `sqlite3*` + `CURL*`, runs blocking `llm_req`) + civetweb
threads. Any of these may hold a malloc-arena / sqlite / curl / stdio lock at
any instant.

Today `fork_tool_exec` does `fork()` (no exec) from this daemon to create the
shell broker, and `tool_shell_handler` then runs **handler logic in that
fork-only image**. A fork-only child of a multithreaded process has one thread
but inherits **all locks in whatever state they were at the fork instant** —
including locks held by *other* daemon threads, now frozen with no owner. The
broker's first `malloc` (in `proxy_bind`, JSON parse, etc.) can deadlock.
"Single-threaded" (thread count) ≠ "clean lock state". The fork-only broker has
the former, not the latter.

`execve` is the only operation that discards inherited locks — it throws away
the whole address space and builds a fresh one. Therefore **every process that
runs real (allocating) logic after forking from the daemon must exec first.**
The file tier already does this. Shell does not. This note makes it uniform.

---

## 2. Minimal-exec analysis (drives the whole structure)

Count execs forced by a real boundary — clean lock state, or a different
program:

| Tier  | exec #1 (sterilize) | inner fork | inner exec (foreign program) | Total |
|-------|---------------------|------------|------------------------------|-------|
| file  | yes → `--run-tool`  | none       | none                         | **1** |
| web   | yes → `--run-tool`  | yes (fork) | none — child runs OUR curl   | **1** |
| js    | yes → `--run-tool`  | yes (fork) | none — child runs OUR qjs    | **1** |
| shell | yes → `--run-tool`  | yes (fork) | yes — `execl /bin/sh`        | **2** |

Key consequences:

- **One exec from the daemon, into one entry point (`--run-tool`), for every
  tier.** That single exec sterilizes inherited locks. There is **no separate
  `--broker` or `--web-fetch` mode** — the "broker" is a *role the `--run-tool`
  process plays for network tiers*, a code branch, NOT a process image. Becoming
  the broker costs **zero** extra execs.
- **web and js sandbox children are plain `fork`s, not fork+exec.** They run
  first-party C (web: `http_do` + `html_to_markdown`; js: `tool_js_eval_handler`
  via QuickJS) and fork from the already-sterile `--run-tool` process
  (single-threaded, clean locks), so the child fork is the safe
  single-threaded-fork case. No exec needed to run our own functions.
- **shell is the ONLY tier with a second exec**, and only because `/bin/sh` is a
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
         verify_clean()                ← shared: no key, fd-set=={0,1,2,3}, no DB
         read + parse request blob from fd 3
         desc = tier_descriptor(blob.tier)   ← {skip_pid_ns, needs_proxy, inner_exec, run_fn}
         if !desc.needs_proxy:               // FILE
             setup_ns(desc.skip_pid_ns)      // no_net, scoped mount set
             result = desc.run_fn(blob)      // in-process, this image
             write result → fd 3; _exit
         else:                               // WEB, JS, SHELL
             serve_network_child(desc, blob) // shared helper, see §4
             write result → fd 3; _exit
  └─ daemon tracks ONE pid (the --run-tool process) via g_children[],
     drains its result over fd 3, reaps via existing sigchld path.
```

`--run-tool` intercept is at the **pre-init position** (top of `main`, before any
DB/config/key init — the `--qjs_eval` site, NOT the `--channel` site). This is
what makes "no key, no DB" true by construction; `verify_clean` then proves it.

---

## 4. The shared network helper (the de-dup)

The three network tiers (web, js, shell) differ on only a few small axes;
everything else is shared in one helper (`serve_network_child`):

```
serve_network_child(const TierDescriptor *desc, RunToolParsed *q,
                    unsigned char *body, size_t body_len):
    proxy_bind(&proxy, agent_dir, blob.host_rules, blob.cidr_rules)
        // bind while single-threaded, BEFORE the fork below
    pid = fork()                         // safe: this image is sterile/1-thread
    if child:
        setpgid(0,0)
        wire stdout/stderr → pipe
        setup_ns(desc.skip_pid_ns, ...)  // fail-closed; _exit on sandbox failure
        if desc.inner_exec:
            tool_shell_tier_exec(q)      // execs /bin/sh, never returns
        else:
            desc.run_fn(q)               // ← in-process leaf (web/js)
        _exit(...)
    // parent (the --run-tool/broker process):
    proxy_serve(&proxy)                  // start accept thread AFTER the fork
    drain child stdout with timeout      // shell's existing drain loop, shared
    waitpid(child); kill(-pgrp) on timeout
    proxy_stop(&proxy)
    return result
```

Per-tier inputs to the helper:

| Axis        | file            | web                          | js                           | shell                        |
|-------------|-----------------|------------------------------|------------------------------|------------------------------|
| skip_pid_ns | 1               | 1                            | 1                            | 0 (PID ns)                   |
| needs_proxy | no              | yes                          | yes                          | yes                          |
| inner_exec  | 0               | 0                            | 0                            | 1                            |
| run_fn      | tool_file_tier_run (not via helper) | tool_web_tier_run (curl + html_to_markdown, **in-process**) | tool_js_tier_run (qjs eval, **in-process**) | NULL (`execl /bin/sh`, **inner exec**) |
| /proc       | absent          | absent                       | absent                       | present (PID ns backs it)    |
| proxy reach | n/a             | net_shim + HTTP_PROXY only (curl is ours; no preload) | net_shim + HTTP_PROXY (qjs http_request uses curl) | net_shim + LD_PRELOAD (untrusted subprocess tree) |

Invariant restated: **/proc present IFF PID namespace present.** Mount set
contains the proxy socket IFF the tier runs untrusted subprocesses that dial
network themselves (shell only — web's own curl and js's qjs `http_request` use
HTTP_PROXY to the shim, no socket in the mount set required beyond what
reachability needs).

Adding a future network tier = a new `(skip_pid_ns, run_fn)` pair plugged into
the table. No new process mode, no new exec, no new drain/proxy code.

---

## 5. Tier descriptor (single source of truth)

Decode `blob.tier` ONCE in `run_tool_main` into a small struct; pass it down.
Do not scatter `if tier=="shell"` across call sites.

```c
typedef char *(*RunFn)(const RunToolParsed *q);

typedef struct {
    int   skip_pid_ns;   /* 1 = no CLONE_NEWPID (file/web/js); 0 = PID ns (shell) */
    int   needs_proxy;   /* 1 = bind/serve proxy + inner fork (network tiers) */
    int   inner_exec;    /* 1 = run_fn execs a foreign program (shell) */
    RunFn run_fn;        /* leaf result producer; NULL iff inner_exec */
} TierDescriptor;
```

`tier_descriptor(tier)` is a compile-time table mapping the four tiers to their
axes. "What makes web different from shell" lives in this one table:

```c
static const TierDescriptor table[] = {
    [RUNTOOL_TIER_FILE]  = { .skip_pid_ns = 1, .needs_proxy = 0, .inner_exec = 0, .run_fn = tool_file_tier_run },
    [RUNTOOL_TIER_SHELL] = { .skip_pid_ns = 0, .needs_proxy = 1, .inner_exec = 1, .run_fn = NULL },
    [RUNTOOL_TIER_WEB]   = { .skip_pid_ns = 1, .needs_proxy = 1, .inner_exec = 0, .run_fn = tool_web_tier_run },
    [RUNTOOL_TIER_JS]    = { .skip_pid_ns = 1, .needs_proxy = 1, .inner_exec = 0, .run_fn = tool_js_tier_run },
};
```

---

## 6. Request blob (fd 3) — extends the file-tier protocol

4-byte LE body-length prefix + a flat little-endian binary body, cap
`RUNTOOL_REQUEST_MAX` (32 KB), fail-closed over cap on the serialize path
(`run_tool_serialize_request` returns NULL when `w_finalize` finds the body
exceeds the cap). **Not JSON**: both ends are the same binary, so the format is
a fixed-order sequence of primitives — `u32` (4-byte LE) and `str` (`u32`
length + raw bytes, length 0 = NULL). No escaping, no key scan; secret values
cross as raw bytes with no escape/unescape round-trip. The body opens with a
tier byte (`0`=file, `1`=shell, `2`=web, `3`=js); `run_tool.c` is the canonical
field order. Fields by tier (additive over file):

- all: `tier`, `tool_name`, `arguments`, `env_mode`, `rlimits{nproc,as_mb,cpu_sec}`
- file: `workspace`, `read_paths[]`, `write_paths[]`, `workspace_ro`, `mount_cwd`, `cwd_path`
- web/shell: `host_rules[]` (exact/suffix), `agent_dir` (for proxy socket; the broker partitions grants into host/CIDR rules at `proxy_bind`)
- shell: `command` (with secrets already interpolated by the daemon parent), `timeout`, `secrets[]` (minimal name/value set for env injection)

**Secrets**: resolved by the **daemon parent** before the blob is written. The
sterile `--run-tool`/broker holds no key and never interpolates. This replaces
shell's current `explicit_bzero`-after-fork dance: parent interpolates → blob
carries resolved values → child consumes → no other process retains them.

---

## 7. Sterility gates (shared, all tiers)

`run_tool_main` enforces sterility at process entry, before reading the request.
Fail-closed (`_exit` on violation), production gate not debug-only:

1. **FD-SET**: every fd above `{0,1,2,3}` is blanket-`close()`d (4..`OPEN_MAX`)
   immediately on entry — this *establishes* the invariant rather than auditing
   it, so a daemon fd that leaked past O_CLOEXEC is closed, not merely detected.
2. **KEY-ABSENT**: `verify_clean()` asserts `db_secret_key_loaded() == 0`.
3. **NO-DB**: no DB handle open (true by construction at the pre-init intercept;
   the key-absent check is its observable proxy).

The broker (network tiers) is subject to the same gates — it too holds no key
and no DB. Only the proxy config + request blob cross fd 3.

---

## 8. What stays unchanged

- Deny-by-default egress; `decide()` pipeline (host rules → resolve → IP/CIDR
  floor → metadata carve-out, enforced on BOTH resolve and numeric-CONNECT
  paths). The broker runs `decide()` exactly as today.
- `proxy_bind` / `proxy_serve` split (bind single-threaded, serve after the
  fork), bless set + TTL, resolve-then-dial-literal anti-TOCTOU/rebind. Reused
  verbatim — they move INTO the exec'd broker, their logic is untouched.
- net_shim as the dumb CONNECT forwarder (structurally no resolver → per-hop
  hostname gating is real). shell adds LD_PRELOAD; web does not.
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
   (2026-06). `tool_web_tier_run` (curl + html_to_markdown) runs in-process in
   the inner fork (no inner exec). `serve_network_child` is the shared path for
   all network tiers; shell's pre-flight `http_check_policy` is subsumed by
   per-hop `decide()`.
3. ✅ **Add js as the third network-tier pair** (2026-06).
   `tool_js_tier_run` (QuickJS eval, `http_request` via curl + HTTP_PROXY) on
   the same `serve_network_child` path. Dispatch in `src/main.c` for both
   `js_eval` and JS-defined extension tools routes through the `SBX_JS` tier.
4. file tier already on the exec'd path; shares `verify_clean` and the
   tier-descriptor dispatch (no separate code path).

---

## 10. Open items

- shell `kill(-pgrp)` across the PID-ns boundary on timeout — verify it reaches
  the inner tree.
- Global egress floor (Q2 from egress-filter.md) + PSL-aware suffix matching
  (Q1) remain deferred; the broker reads whatever rules cross fd 3.
