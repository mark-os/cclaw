# Daemon/Agent Separation Refactor

## Problem Statement

CClaw currently uses a single shared SQLite DB for all state (daemon coordination, agent sessions, inbox, config). Agents write directly to shared tables (spawn_queue, approvals, kv) creating tight coupling. This refactor cleanly separates concerns following Unix principles: daemon as init/service-broker, agents as isolated users with home directories, processes as cheap/disposable, communication via exit codes + DB reads.

## Requirements

1. Three DB files: `daemon.db` (registry, permissions, cron, spawn_queue, channels, provider config), `journal.db` (all logs from daemon + agents), per-agent `agents/<name>/agent.db` (sessions, entries, inbox, js_tools, memory_blocks)
2. No JSON config files — all config in SQLite (daemon.db for permissions/policy, agent env vars for runtime)
3. Strict process separation — agents write only to their own DB, communicate intent via exit codes
4. Daemon reads agent DB after reap to discover requests (spawn, approval, config change)
5. Daemon writes to agent DBs only for inbox delivery
6. Log collector process — receives all stdout/stderr via pipes, writes to journal.db
7. Config injected to agents via `CCLAW_*` env vars at fork time
8. Agents can optionally have read-only access to daemon.db (landlock-granted)
9. CLI remains standalone (opens agent DB directly, no daemon needed)
10. No backward compatibility — no migrations, no legacy support

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        DAEMON                                │
│  daemon.db (registry, permissions, cron, spawn_queue,        │
│             channel_bindings, provider config)                │
│  Threads: Telegram poller, civetweb, cron scheduler,         │
│           heartbeat                                          │
│  Epoll: signal pipe + SIGCHLD                                │
│  Forks: agent processes (exit code signaling)                │
│  Spawns: log collector (once, at startup)                    │
└──────┬──────────────────────────────────┬────────────────────┘
       │ fork+exec                        │ pipe stdout/stderr
       ▼                                  ▼
┌──────────────┐                 ┌─────────────────┐
│ Agent proc   │                 │  Log Collector   │
│ agent.db     │                 │  journal.db      │
│ (own DB)     │                 │  epoll on pipes  │
│ setrlimit    │                 │  writes all logs │
│ exit code    │                 └─────────────────┘
│ signals      │
│ intent       │
└──────────────┘

┌──────────────┐
│     CLI      │
│ standalone   │
│ opens agent  │
│ DB directly  │
│ no daemon    │
└──────────────┘
```

## Exit Code Protocol

| Code | Meaning | Daemon Action |
|------|---------|---------------|
| 0 | Turn complete, deliver response | Read last assistant entry from agent DB, deliver to channel |
| 1 | Turn complete with error | Log error, mark session idle |
| 2 | Spawn sub-agent requested | Read last tool_call from agent DB, fork child |
| 3 | Approval requested | Read last tool_call from agent DB, notify admin |
| 4 | Config change requested | Read last tool_call from agent DB, validate + apply |
| 127 | exec failed | Log error |
| 128+N | Killed by signal N | Log crash, synthesize error |

## Env-Var Config Injection

Daemon reads `agent_config` table from daemon.db at fork time. Injects as env vars:

| Env Var | Source | Notes |
|---------|--------|-------|
| `CCLAW_AGENT_NAME` | agent identity | |
| `CCLAW_AGENT_DB` | path to agent.db | |
| `CCLAW_WORKSPACE` | agent_config.workspace | |
| `CCLAW_MODEL` | agent_config.model | |
| `CCLAW_MAX_ITERATIONS` | agent_config.max_iterations | |
| `CCLAW_ALLOWED_HOSTS` | agent_config.allowed_hosts | comma-separated |
| `CCLAW_TOOLS` | agent_config.tools | comma-separated |
| `CCLAW_SHELL_TIMEOUT` | agent_config.shell_timeout | seconds |
| `CCLAW_INJECTED_API_KEY` | daemon.db kv (decrypted) | provider API key — cleared after read |
| `CCLAW_DAEMON_DB` | daemon.db path | only if daemon_db_read=1 |

Agent reads env vars at startup. ⊥ opens config files. ⊥ opens daemon.db for config.

## Trust Model

See [specs/security.md](specs/security.md) for full details.

- **Agent process**: trusted binary, config-constrained. Primary isolation: `setrlimit` + application-level `http_check_policy()`. Optional landlock as defense-in-depth.
- **Shell children**: untrusted. Namespace sandbox (`CLONE_NEWUSER|CLONE_NEWNS|CLONE_NEWNET`) + transparent credential proxy. See [specs/shell-networking.md](specs/shell-networking.md).

## Task Tracking

All implementation tasks live in [SPEC.md §T](SPEC.md). Current phases:

- **Phase 1** (T198-T201): Core DB split — agent isolation, daemon inbox writes, session state, reap dispatch
- **Phase 2** (T202-T205): Daemon coordination — spawn_queue, approvals, cron, bootstrap
- **Phase 3** (T206-T207): CLI standalone + integration tests
- **Phase 4** (T208-T217): Shell namespace sandbox + credential proxy
- **Phase 5** (T218-T219): Optional hardening (log collector, landlock)
