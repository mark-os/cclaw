# Daemon/Agent Separation Refactor

## Problem Statement

CClaw currently uses a single shared SQLite DB for all state (daemon coordination, agent sessions, inbox, config). Agents write directly to shared tables (spawn_queue, approvals, kv) creating tight coupling. This refactor cleanly separates concerns following Unix principles: daemon as init/service-broker, agents as isolated users with home directories, processes as cheap/disposable, communication via exit codes + DB reads.

## Requirements

1. Three DB files: `cclaw.db` (registry, permissions, cron, spawn_queue, channels, provider config), `journal.db` (all logs from daemon + agents), per-agent `agents/<name>/agent.db` (sessions, entries, inbox, js_tools, memory_blocks)
2. Config in SQLite (`cclaw.db`) with optional `config.json` fallback for convenience
3. Strict process separation — agents write only to their own DB, communicate intent via exit codes
4. Daemon reads agent DB after reap to discover requests (spawn, approval, config change)
5. Daemon writes to agent DBs only for inbox delivery
6. Log collector process — receives all stdout/stderr via pipes, writes to journal.db
7. Config injected to agents via `CCLAW_*` env vars at fork time
8. Agents can optionally have read-only access to cclaw.db (landlock-granted)
9. CLI remains standalone (opens agent DB directly, no daemon needed)
10. No backward compatibility — no migrations, no legacy support

## Architecture

```
~/.cclaw/
├── cclaw.db           ← system registry + config (all modes read this)
├── .cclaw_key         ← 32-byte encryption key (mode 0600)
├── journal.db         ← all logs (daemon + agents)
├── config.json        ← optional convenience fallback
└── agents/
    └── <name>/
        ├── agent.db   ← sessions, entries, inbox, memory, js_tools
        └── workspace/ ← agent-created files
```

```
┌─────────────────────────────────────────────────────────────┐
│                        DAEMON                                │
│  cclaw.db (registry, permissions, cron, spawn_queue,         │
│            channel_bindings, provider config)                 │
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

## Config Resolution

The agent process only ever reads env vars. Config resolution happens *before* the agent loop starts (in CLI entrypoint or daemon fork):

### API Key Resolution (per provider)

1. `CCLAW_OPENROUTER_API_KEY` env var (explicit CClaw-namespaced, highest priority)
2. `cclaw.db` kv table: `openrouter_api_key` → `enc:<ciphertext>` (decrypted on read)
3. `OPENROUTER_API_KEY` env var (system-level fallback — respects existing user setup)
4. `~/.cclaw/config.json` field `"openrouter_api_key"` (convenience fallback)

Same pattern for `gemini_api_key`, `anthropic_api_key`, `telegram_token`, etc.

Once resolved, injected to agent as the provider-native env var (e.g. `OPENROUTER_API_KEY`). Agent reads via `getenv(cfg->provider.api_key_env)`.

### General Config Resolution

1. `CCLAW_*` env vars (highest priority — Lambda, Workers, daemon fork all use this)
2. `cclaw.db` `agent_config` table (per-agent) and `kv` table (global)
3. `~/.cclaw/config.json` (optional fallback for users who prefer a file)

### First-Run Flow

1. User runs `cclaw` — no `~/.cclaw/` exists
2. Create `~/.cclaw/`, generate `.cclaw_key` (32 random bytes, mode 0600)
3. Create `cclaw.db` with default schema
4. If `OPENROUTER_API_KEY` in env: encrypt, store in cclaw.db kv as `openrouter_api_key: enc:...`
5. Create `agents/default/agent.db` + `workspace/`
6. Enter agent loop — user is chatting immediately

### config.json Format

Optional file at `~/.cclaw/config.json`. Simple flat key-value:

```json
{
  "openrouter_api_key": "sk-or-v1-...",
  "model": "deepseek/deepseek-v4-flash",
  "telegram_token": "123456:ABC..."
}
```

Read by CLI/daemon at startup as lowest-priority fallback. Never read by agent processes.

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

Daemon reads `agent_config` table from cclaw.db at fork time. Injects as env vars:

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
| Provider API key | cclaw.db kv (decrypted) | injected as native env var (e.g. `OPENROUTER_API_KEY`) |
| `CCLAW_DAEMON_DB` | cclaw.db path | only if daemon_db_read=1 |

Agent reads env vars at startup. ⊥ opens config files. ⊥ opens cclaw.db for config.

## Trust Model

See [specs/security.md](specs/security.md) for full details.

- **Agent process**: trusted binary, config-constrained. Primary isolation: `setrlimit` + application-level `http_check_policy()`. Optional landlock as defense-in-depth.
- **Shell children**: untrusted. Namespace sandbox (`CLONE_NEWUSER|CLONE_NEWNS|CLONE_NEWNET`) + transparent credential proxy. See [specs/shell-networking.md](specs/shell-networking.md).

## Task Tracking

All implementation tasks live in [SPEC.md §T](SPEC.md). Current phases:

- **Phase 1** (T195-T201): Core DB split — agent isolation, daemon inbox writes, session state, reap dispatch
- **Phase 2** (T202-T205): Daemon coordination — spawn_queue, approvals, cron, bootstrap
- **Phase 3** (T206-T207): CLI standalone + integration tests
- **Phase 4** (T208-T217): Shell namespace sandbox + credential proxy
- **Phase 5** (T218-T219): Optional hardening (log collector, landlock)
