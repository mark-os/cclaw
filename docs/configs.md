# Configuration Reference

## Resolution Order

Config is resolved highest-priority-first:

1. **`CCLAW_*` env vars** — read once at process start
2. **cclaw.db `config` table** — persistent operator/agent settings
3. **`OPENROUTER_API_KEY` env var** — system-level API key fallback

Config is loaded once at startup. Worker threads inherit the loaded config.

## Global Config (cclaw.db `config` table)

The `config` table is registry-backed: every key is declared in the C
registry (`src/config_registry.c`) with a code-owned `default_value` and
`description`, synced into the table at startup. Operators override by
setting `value`; anonymous keys are rejected. Extensions may register
additional keys as `<ext>.<key>` via their manifest's `config[]` section
(see specs/extensions.md).

The registry is the authoritative key list — don't duplicate it here.
Agents discover it at runtime via the `search_config` tool; operators via
`sqlite3 "$DB" "SELECT key, COALESCE(value, default_value), description FROM config"`.

## Per-Agent Config

There is no per-agent key/value table. Per-agent state lives in:

- **`agents` table columns** — `sandbox_profile` (containment tier: `host`,
  `trusted`, `standard`, `restricted`), model/provider overrides, system
  prompt, `max_iterations`, `max_output_tokens`, `shell_timeout`.
- **`grants` table rows** — authority: which tools, hosts, and paths the
  agent may use. Missing grant = not allowed; agents request more via
  `request_config` (human-approved).
- **`agent_extensions` rows** — which extensions are attached (attach never
  grants authority; grants stay per-tool, per-agent).

See specs/trust.md for the containment/authority model and specs/schema.md
for the tables.

## Providers

Provider endpoints, models, and priorities are rows in the `providers` and
`models` tables (seeded from `templates/seed.sql`; managed at runtime via
the `configure_provider` tool). API keys are stored encrypted in the
`secrets` table.

## File Layout

```
~/.cclaw/
├── cclaw.db           ← all state (sessions, entries, config, memory, channels)
├── .cclaw_key         ← encryption key for secrets (mode 0600)
├── extensions/        ← shared extension store (promoted bundles)
└── agents/
    └── <name>/
        └── workspace/ ← agent-created files
```

Override the DB location with `CCLAW_DB_PATH`.

## Quick Start

```bash
export OPENROUTER_API_KEY="sk-or-v1-..."
./build/cclaw
```

Defaults: OpenRouter, DeepSeek V4 Flash, minimal tool grants — agents
request more via `request_config`.
