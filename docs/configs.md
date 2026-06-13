# Configuration Reference

## Resolution Order

Config is resolved highest-priority-first:

1. **`CCLAW_*` env vars** — works everywhere (daemon, CLI, Lambda)
2. **cclaw.db `kv` table** — persistent config, encrypted secrets
3. **`OPENROUTER_API_KEY` env var** — system-level API key fallback

Config is loaded once at startup from env vars and cclaw.db. Worker threads inherit the loaded config.

## Global Config (cclaw.db `kv` table)

System-wide settings. Per-agent overrides take precedence.

| kv key | Env var | Default | Description |
|--------|---------|---------|-------------|
| `provider.base_url` | `CCLAW_PROVIDER_BASE_URL` | `https://openrouter.ai/api/v1` | LLM API base URL |
| `provider.model` | `CCLAW_MODEL` | `deepseek/deepseek-v4-flash` | Model identifier |
| `provider.api_key` | via `CCLAW_PROVIDER_API_KEY_ENV` | — | API key (encrypted in DB) |
| `provider.max_tokens` | `CCLAW_MAX_TOKENS` | 4096 | Max response tokens |
| `provider.context_window` | `CCLAW_CONTEXT_WINDOW` | 65536 | Context window size |
| `default_agent` | — | `default` | Agent used by `-p` flag |
| `log_level` | `CCLAW_LOG_LEVEL` | `info` | error\|info\|debug\|trace |
| `token_rate_limit` | `CCLAW_TOKEN_RATE_LIMIT` | 1000000 | Max tokens/hour (0=unlimited) |
| `web_port` | `CCLAW_WEB_PORT` | 8080 | Dashboard port (daemon mode) |
| `heartbeat_interval` | `CCLAW_HEARTBEAT_INTERVAL` | 0 | Heartbeat seconds (0=disabled) |
| `telegram_token` | — | — | Bot token (encrypted in DB) |
| `admin_chat_ids` | — | `[]` | JSON array of admin Telegram chat IDs |
| `fallback_providers` | — | `[]` | JSON array of fallback provider configs |
| — | `CCLAW_SAVE_REASONING` | 0 | Store reasoning in metadata |
| — | `CCLAW_SAVE_USAGE` | 0 | Store token usage in metadata |
| — | `OPENROUTER_API_KEY` | — | Fallback API key (if no provider.api_key) |

## Per-Agent Config (cclaw.db `agent_config` table)

Each agent has config stored as key-value rows in `agent_config(agent_name, key, value)`. Missing keys use conservative system defaults — not "allow everything."

| Key | Env var | Default | Description |
|-----|---------|---------|-------------|
| `model` | `CCLAW_MODEL` | global model | Model override |
| `workspace` | `CCLAW_WORKSPACE` | `agents/<name>/workspace` | Agent workspace path |
| `tools` | `CCLAW_TOOLS` | see below | Comma-separated tool whitelist |
| `allowed_hosts` | `CCLAW_ALLOWED_HOSTS` | (empty — no network) | Comma-separated hostnames |
| `max_iterations` | `CCLAW_MAX_ITERATIONS` | 25 | Tool loop cap |
| `shell_timeout` | `CCLAW_SHELL_TIMEOUT` | 30 | Shell exec timeout (seconds) |
| `memory_limit` | `CCLAW_MEMORY_LIMIT` | 268435456 (256MB) | RLIMIT_AS bytes; 0=unlimited |
| `cpu_limit` | `CCLAW_CPU_LIMIT` | 300 | RLIMIT_CPU seconds; 0=unlimited |
| `read_access` | — | `[]` | Extra dirs for read-only access |

### Default Tool Set

New agents start with minimum tools:

```
file_read, file_write, js_eval, memory_create, memory_append, memory_replace, request_config
```

The agent can request additional tools (shell_exec, web_fetch, db_query, js_define_tool) via `request_config` — user approves inline in CLI mode.

### Context & Compaction

| Env var | Default | Description |
|---------|---------|-------------|
| `CCLAW_CONTEXT_THRESHOLD` | 0.6 | Fraction of context_window that triggers action |
| `CCLAW_COMPACTION_TARGET` | 0.3 | Post-compaction target (fraction) |
| `CCLAW_COMPACTION` | 1 | Enable compaction (0=truncate only) |
| `CCLAW_AUTO_RECALL` | 1 | FTS5 auto-recall from past sessions |
| `CCLAW_RECALL_MAX_TOKENS` | 500 | Max recalled context tokens |

## Network Policy

Controlled by `allowed_hosts` in agent_config. Shared by web_fetch and shell_exec networking.

- **`allowed_hosts` empty (default)**: no outbound network — agent must request hosts via `request_config`
- **`allowed_hosts` set**: only listed hosts reachable (both web_fetch and shell proxy)
- **Private IPs**: always blocked (RFC1918, loopback, link-local, cloud metadata)

## CLI-Only Settings

Set by the CLI process, not from agent_config:

| Env var | Description |
|---------|-------------|
| `CCLAW_MODE=cli` | Enables progress output, reduced tool set |
| `CCLAW_PATH` | CWD path for read-only file access |
| `CCLAW_STREAM=1` | SSE streaming for real-time token output |
| `CCLAW_YOLO=1` | `-y` flag: disables sandbox, allows all hosts |

## File Layout

```
~/.cclaw/
├── cclaw.db           ← all state (sessions, entries, config, memory, channels)
├── .cclaw_key         ← encryption key for secrets (mode 0600)
└── agents/
    ├── default/
    │   └── workspace/ ← agent-created files
    └── researcher/
        └── workspace/
```

## Quick Start

```bash
export OPENROUTER_API_KEY="sk-or-v1-..."
./build/cclaw
```

All defaults: OpenRouter, DeepSeek V4 Flash, minimal tools, 25 iterations, 30s shell timeout.

## Example: Restricted Agent

```sql
INSERT INTO agent_config (agent_name, key, value) VALUES
  ('researcher', 'model', 'anthropic/claude-sonnet-4-20250514'),
  ('researcher', 'tools', '["file_read","file_write","web_fetch","memory_create","memory_append","memory_replace"]'),
  ('researcher', 'allowed_hosts', '["api.github.com","arxiv.org"]'),
  ('researcher', 'max_iterations', '50');
```

This agent can fetch from GitHub/arxiv, write files, use memory. No shell access.

## Example: Powerful Build Agent

```sql
INSERT INTO agent_config (agent_name, key, value) VALUES
  ('builder', 'tools', '["shell_exec","file_read","file_write","web_fetch","js_eval","memory_create","memory_append","memory_replace"]'),
  ('builder', 'allowed_hosts', '["registry.npmjs.org","github.com"]'),
  ('builder', 'memory_limit', '0'),
  ('builder', 'cpu_limit', '0'),
  ('builder', 'max_iterations', '100');
```

Unlimited memory/CPU, shell access, network to npm + GitHub.
