# Shell Trust Levels — Per-Agent Sandbox Profiles

Plan for hanging shell lockdown policy off the existing `agents.trust_level`
column instead of adding per-knob columns. Complements specs/security.md.

## Current state (2026-06)

Every sandboxed `shell_exec` child already gets (src/tool_shell.c):

- user + mount + PID + **net** namespaces; netns has no interfaces, so the
  UDS credential proxy (`CCLAW_PROXY_SOCK`) is the only egress, and the
  parent enforces `allowed_hosts` on it
- `pivot_root` into throwaway tmpfs; `/bin /usr /lib /etc /proc /dev /sbin`
  bind-mounted read-only; workspace rw; CLI CWD rw
- env hardening: PATH=/bin:/usr/bin, HOME unset, `CCLAW_*` unset, generic
  credential scrub (names containing API_KEY/APIKEY/TOKEN/SECRET/PASSWORD/
  CREDENTIALS); `CCLAW_SECRET_*` re-injected as the deliberate channel
- `-y` (yolo) disables all of the above

Per-agent knobs that exist today:

- `allowed_tools` — withholding `shell_exec` is the strongest lockdown
- `allowed_hosts` — gates the proxy = gates shell egress; **empty list
  currently means allow-all**, so "no network" is not expressible
- `trust_level` — column exists (`standard`, `bootstrap`), carries almost
  no policy yet

## Known gaps

1. Env is inherit-minus-blacklist; novel names (`DATABASE_URL`, lowercase
   secrets) leak through.
2. No way to express "no network at all".
3. CLI bind-mounts the user's CWD rw for every agent.
4. No fork-bomb / memory limits (only shell_timeout).
5. Workspace is always rw; no read-only mode for observer agents.

## Plan: trust_level → policy bundle

Resolve policy in ONE place (agent_setup_init → ShellConfig), not scattered.

| Policy                  | trusted          | standard (default) | restricted |
|-------------------------|------------------|--------------------|------------|
| env                     | inherit+scrub    | clean allowlist    | clean allowlist |
| network                 | proxy            | proxy              | none (no proxy sock) |
| CWD mount (CLI)         | rw               | no                 | no         |
| workspace mount         | rw               | rw                 | ro         |
| rlimits (NPROC/AS/CPU)  | none             | generous           | tight      |

- `trusted` = today's behavior (current default agent keeps working;
  `bootstrap` maps to `trusted`)
- **clean allowlist env**: `clearenv()`, then set only PATH, TMPDIR,
  CCLAW_PROXY_SOCK, and `CCLAW_SECRET_*` injections (~10 lines in the
  child setup; replaces the blacklist scrub for these levels)
- **network none**: skip setting CCLAW_PROXY_SOCK and don't start/expose
  the proxy for this agent; empty netns then has zero egress. Decide
  whether to also flip `allowed_hosts` semantics (`["none"]` sentinel) or
  let trust_level alone carry it.

## Implementation steps

1. `include/tool_shell.h` ShellConfig: add `env_mode`, `net_mode`,
   `mount_cwd`, `workspace_ro`, rlimit values.
2. `src/agent_setup.c agent_setup_init`: read `agents.trust_level`, map to
   the bundle above, fill ShellConfig. Unknown values → `standard`.
3. `src/tool_shell.c` child setup:
   - env_mode clean → clearenv()+allowlist (replaces scrub block)
   - workspace_ro → add MS_RDONLY on the workspace bind remount
   - mount_cwd=0 → skip the CWD bind-mount block
   - setrlimit() calls before execl
4. `proxy`/`agent_setup`: net_mode none → don't pass proxy_sock.
5. Tests: per-level env visibility, no-egress (proxy sock absent), CWD
   absent in restricted, ro workspace write fails, NPROC limit enforced.
6. Docs: update specs/security.md trust table; AGENTS.md note on choosing
   a trust_level when creating agents.

## Non-goals

- seccomp syscall filtering — maintenance tax, namespaces+proxy already
  cover the realistic threats; revisit only with a concrete bypass in hand.
- Container/VM isolation — the shell still runs as the user's uid under
  the shared kernel; that boundary is out of scope for cclaw itself.
