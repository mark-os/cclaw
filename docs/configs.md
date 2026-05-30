# Configuration Reference

## Resolution Order

Config is resolved highest-priority-first. A value set at a higher level overrides lower levels.

1. **`CCLAW_*` env vars** — works everywhere (daemon fork, Lambda, Workers, CLI)
2. **cclaw.db `kv` table** — persistent config, encrypted secrets (daemon mode)
3. **`OPENROUTER_API_KEY` env var** — system-level API key fallback
4. **`~/.cclaw/config.json`** — optional convenience file (lowest priority)

In daemon mode, the daemon reads config from cclaw.db, then forks agent processes with `CCLAW_*` env vars injected. Agent processes only read env vars — they never open cclaw.db for config.

## Global Config (cclaw.db `kv` table)

These apply system-wide. Per-agent overrides (below) take precedence.

| kv key | Env var | Default | Description |
|--------|---------|---------|-------------|
| `provider.base_url` | `CCLAW_PROVIDER_BASE_URL` | `https://openrouter.ai/api/v1` | LLM API base URL |
| `provider.model` | `CCLAW_MODEL` | `deepseek/deepseek-v4-flash` | Model identifier |
| `provider.api_key` | via `CCLAW_PROVIDER_API_KEY_ENV` | — | API key (encrypted in DB) |
| `provider.max_tokens` | `CCLAW_MAX_TOKENS` | 4096 | Max response tokens |
| `provider.context_window` | `CCLAW_CONTEXT_WINDOW` | 65536 | Context window size |
| `provider.cache_hints` | — | `auto` | Cache hint mode: `on`, `off`, `auto` |
| — | `OPENROUTER_API_KEY` | — | Fallback API key (if no provider.api_key) |
| `workspace` | `CCLAW_WORKSPACE` | `.cclaw/agents/default/workspace` | Default workspace path |
| `max_iterations` | `CCLAW_MAX_ITERATIONS` | 25 | Agent loop iteration cap |
| `max_history_tokens` | `CCLAW_MAX_HISTORY_TOKENS` | 0 (60% of context_window) | Token budget for context |
| `shell_timeout` | `CCLAW_SHELL_TIMEOUT` | 30 | Default shell_exec timeout (seconds) |
| `token_rate_limit` | `CCLAW_TOKEN_RATE_LIMIT` | 1000000 | Max tokens/hour (0=unlimited) |
| `stale_lock_timeout` | — | 300 | Janitor stale lock threshold (seconds) |
| `heartbeat_interval` | `CCLAW_HEARTBEAT_INTERVAL` | 0 | Heartbeat interval (0=disabled) |
| `web_port` | `CCLAW_WEB_PORT` | 8080 | Civetweb dashboard port |
| `telegram_token` | `CCLAW_TELEGRAM_TOKEN` | — | Telegram bot token (encrypted in DB) |
| `admin_chat_ids` | — | `[]` | JSON array of admin Telegram chat IDs |
| `fallback_providers` | — | `[]` | JSON array of fallback provider configs |
| — | `CCLAW_SAVE_REASONING` | 0 | Store reasoning tokens in metadata |
| — | `CCLAW_SAVE_USAGE` | 0 | Store token usage in metadata |
| — | `CCLAW_SAVE_LOGPROBS` | 0 | Store logprobs in metadata |
| — | `CCLAW_DEBUG` | 0 | Dump raw LLM JSON to stderr |

## Per-Agent Config (cclaw.db `agent_config` table)

Each agent has its own config stored as key-value rows in the `agent_config` table (keyed by agent name). These override global config for that agent.

| Key | Env var (injected at fork) | Default | Description |
|-----|---------------------------|---------|-------------|
| `model` | `CCLAW_MODEL` | global model | Model override |
| `workspace` | `CCLAW_WORKSPACE` | `.cclaw/agents/<name>/workspace` | Agent workspace path |
| `max_iterations` | `CCLAW_MAX_ITERATIONS` | global value | Iteration cap override |
| `tools` | `CCLAW_TOOLS` | all tools | Comma-separated tool names (whitelist) |
| `allowed_hosts` | `CCLAW_ALLOWED_HOSTS` | `[]` (all allowed) | Comma-separated hostnames for HTTP |
| `read_access` | — | `[]` | Extra dirs for read-only namespace access |

The daemon also injects these identity env vars at fork:

| Env var | Description |
|---------|-------------|
| `CCLAW_AGENT_NAME` | Agent name (from directory) |
| `CCLAW_AGENT_DB` | Path to agent's agent.db |

## Tools

All tools are optional. By default, all registered tools are available to the agent. If a `tools` array is specified in agent_config, only those tools are exposed to the LLM.

### Default Tool Set

| Tool | Mode | Description |
|------|------|-------------|
| `shell_exec` | CLI + Daemon | Run shell commands (namespace-sandboxed) |
| `file_read` | CLI + Daemon | Read files (workspace-scoped) |
| `file_write` | CLI + Daemon | Write files (workspace-scoped) |
| `js_eval` | CLI + Daemon | Evaluate JavaScript (MicroQuickJS) |
| `js_define` | CLI + Daemon | Define persistent JS tools |
| `web_fetch` | CLI + Daemon | Fetch URLs (host-allowlist enforced) |
| `db_query` | CLI + Daemon | Read-only SQL against agent.db |
| `memory` | CLI + Daemon | Read/write memory blocks |
| `approval_request` | Daemon only | Request human approval |
| `configure_provider` | Daemon only | Set up LLM provider |
| `configure_channel` | Daemon only | Set up Telegram/CLI channel |
| `create_agent` | Daemon only | Create a new agent |
| `launch_agent` | Daemon only | Spawn a sub-agent |
| `check_agent` | Daemon only | Check sub-agent status |

### Tool Whitelist

To restrict an agent to specific tools, set the `tools` key in agent_config:

```json
["shell_exec", "file_read", "file_write"]
```

When `tools` is set, only listed tools are sent to the LLM in the request schema. Unlisted tools are still registered (for internal use) but invisible to the model.

When `tools` is NULL or empty, all registered tools are available.

## Network Policy

Outbound HTTP from the agent process is controlled by `http_check_policy()`:

- **`allowed_hosts` non-empty**: only listed hosts reachable (default-deny)
- **`allowed_hosts` empty**: all hosts reachable except `blocked_hosts` (default-allow)
- **`block_private`**: blocks RFC1918, loopback, link-local, cloud metadata (default: true)

Shell children run in a network namespace with no connectivity. Network access is proxied back through the agent via UDS — the agent's host allowlist applies.

## File Layout

```
~/.cclaw/
├── cclaw.db           ← system registry + global config (kv table)
├── .cclaw_key         ← encryption key for secrets (mode 0600)
├── journal.db         ← all logs
├── config.json        ← optional lowest-priority fallback
└── agents/
    ├── default/
    │   ├── agent.db   ← sessions, entries, memory
    │   ├── workspace/ ← agent-created files
    │   ├── system.md  ← custom system prompt (optional)
    │   └── skills/    ← skill .md files (optional)
    └── coder/
        ├── agent.db
        ├── workspace/
        └── ...
```

## Example: Minimal Setup

```bash
export OPENROUTER_API_KEY="sk-or-v1-..."
./build/cclaw
```

Uses all defaults: OpenRouter, DeepSeek V4 Flash, all tools enabled, 25 iterations, 30s shell timeout.

## Example: Restricted Agent (via cclaw.db)

```sql
-- In cclaw.db agent_config table:
INSERT INTO agent_config (agent, key, value) VALUES
  ('researcher', 'model', 'anthropic/claude-sonnet'),
  ('researcher', 'tools', '["web_fetch", "file_write", "memory"]'),
  ('researcher', 'allowed_hosts', '["api.github.com", "arxiv.org"]'),
  ('researcher', 'max_iterations', '50');
```

This agent can only fetch from GitHub/arxiv, write files, and use memory. No shell access.

## Hardcoded (Not Configurable)

These are baked into the binary at build time. Change by editing source/templates and rebuilding.

| What | Value | Where |
|------|-------|-------|
| Default system prompt | `templates/default_system_prompt.md` | Overridden by `agents/<name>/system.md` |
| Cutoff notice text | `templates/cutoff_notice.txt` | Shown when context is truncated |
| DB schemas | `templates/schema_*.sql` | Applied on first DB open |
| Shell skill instructions | `templates/skill_shell.md` | Always injected into system prompt |
| Max LLM retries | 5 | `src/agent.c` `MAX_RETRIES` |
| Retry backoff | 1s, 2s, 4s, 8s, 16s | Exponential, respects `Retry-After` header |
| setrlimit caps | 256MB memory, 300s CPU, 64 fds | `src/main.c` (agent process) |
| Token estimate | `strlen(content) / 4` | Stored per-entry at write time |
| Namespace sandbox paths | `/bin`, `/usr`, `/lib`, `/etc`, `/proc`, `/dev` (ro) | `src/tool_shell.c` |

See [docs/templating.md](templating.md) for details on the template system.
