# Architecture

## Process Model

CClaw has two execution modes sharing the same binary:

**CLI mode** (default): single process, runs the agent loop directly. No daemon, no threads (except the credential proxy for shell children). Opens `~/.cclaw/agents/default/agent.db` and talks to the LLM.

**Daemon mode** (`--daemon`): epoll loop that forks isolated agent processes per session turn. Manages channels (Telegram, webhooks), cron, and the web dashboard. Never executes LLM logic itself.

```
Daemon (optional)                    Agent process (forked or standalone)
├── Telegram poller thread           ├── Opens own agent.db (RW)
├── Civetweb thread (webhooks)       ├── setrlimit (memory/CPU caps)
├── Cron scheduler thread            ├── Drains inbox → builds context
├── Heartbeat thread                 ├── LLM call (libcurl)
├── Epoll: signal pipe + SIGCHLD     ├── Tool dispatch loop
│                                    ├── Writes response to DB
└── Forks agent on inbox signal      └── exit(code) — signals intent
```

## Exit Code Protocol

Agent processes communicate intent to the daemon via exit codes. The daemon reads details from the agent's DB after reap.

| Code | Meaning | Daemon Action |
|------|---------|---------------|
| 0 | Turn complete | Deliver last assistant entry to channel |
| 1 | Error | Log error, mark session idle |
| 2 | Spawn sub-agent | Read tool_call args, fork child agent |
| 3 | Approval needed | Read tool_call args, notify admin |
| 4 | Config change | Read tool_call args, validate + apply |
| 127 | exec failed | Log error |
| 128+N | Killed by signal N | Log crash, synthesize error entry |

In CLI mode, exit codes are unused — the process handles everything inline.

## First-Run Flow

1. User runs `cclaw` — no `~/.cclaw/` exists
2. Create `~/.cclaw/`, generate `.cclaw_key` (32 random bytes, mode 0600)
3. Create `cclaw.db` with schema, seed default config
4. If `OPENROUTER_API_KEY` in env → encrypt and store in cclaw.db kv
5. Create `agents/default/agent.db` + `workspace/`
6. Enter agent loop — user is chatting immediately

Total first-run overhead: ~165ms (schema creation + WAL init).

## 3-DB Split

```
cclaw.db (daemon-owned)           Per-agent agent.db              journal.db (collector-owned)
├── agents registry               ├── sessions                    └── log (all stdout/stderr)
├── agent_config (policy)         ├── entries (messages)
├── providers                     ├── inbox
├── kv (encrypted secrets)        ├── js_tools
├── channel_bindings              ├── memory_blocks
├── spawn_queue                   └── kv (local prefs)
├── cron_jobs
└── approvals
```

**Separation principle**: agents write only to their own DB. Daemon writes to agent DBs only for inbox delivery. Cross-agent access is impossible (namespace sandbox hides other paths).

## Config Injection

Daemon reads agent config from cclaw.db at fork time, injects as `CCLAW_*` env vars. Agent processes only read env vars — never open cclaw.db for config.

See [docs/configs.md](configs.md) for the full config reference.

## Security Layers

| Layer | Protects | Enforced by |
|-------|----------|-------------|
| setrlimit | Resource exhaustion | Kernel (agent process) |
| http_check_policy | Network exfiltration | Application (agent process) |
| Namespace sandbox | Filesystem + network | Kernel (shell children) |
| UDS proxy | Host allowlist for shell | Application (agent process) |
| Env stripping | Secret leakage to shell | Application (agent process) |
| Encrypted secrets | Disk theft | ChaCha20-Poly1305 (cclaw.db) |

See [specs/security.md](../specs/security.md) for full details.
