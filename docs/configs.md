# Configuration Reference

## Resolution Order

Every config key resolves highest-priority-first:

1. **`CCLAW_*` env vars** — the key in uppercase with `.` turned into `_`
   (`telegram.bot_token` → `CCLAW_TELEGRAM_BOT_TOKEN`). Never stored in the
   DB. Since this is the process environment, changing it means restarting
   the process.
2. **cclaw.db `config` table** — the stored override, set by the operator or
   by an approved `request_config` change.
3. **The registered default** — shipped in the code.

Keys flagged *secret* skip step 2: they resolve from the environment or the
encrypted `secrets` table only.

Provider API keys are not config keys. Each `providers` row names the
variable that holds its key (`api_key_env`), looked up in the environment
first and then in the `secrets` table. `OPENROUTER_API_KEY` works out of the
box only because the seeded `openrouter` row names it.

## Global Config (cclaw.db `config` table)

The `config` table is registry-backed: every key is declared in the C
registry (`src/config_registry.c`) with a code-owned `default_value` and
`description`, synced into the table at startup. Operators override by
setting `value`; unregistered keys are rejected. Extensions may register
additional keys as `<ext>.<key>` via their manifest's `config[]` section
(see [specs/extensions.md](../specs/extensions.md)).

The registry is the authoritative key list — don't duplicate it here.
Agents discover it at runtime with the `search_config` tool; operators with:

```bash
cclaw sqlite3 ~/.cclaw/cclaw.db \
  "SELECT key, COALESCE(value, default_value), description FROM config"
```

## Per-Agent Config

There is no per-agent key/value table. Per-agent state lives in:

- **`agents` table columns** — `sandbox_profile` (containment: `host`,
  `standard`, `restricted`), system prompt, `max_iterations`,
  `max_output_tokens`, `shell_timeout`, `shell_path` (interpreter for
  `shell_exec`'s `-c`; NULL = `/bin/sh`; the `CCLAW_SHELL_PATH` env var
  overrides both, e.g. `/bin/bash`).
- **`agent_models` rows** — the agent's model routing list, in order; the
  first entry is primary.
- **`grants` table rows** — authority: which tools, hosts, and paths the
  agent may use. Missing grant = not allowed; agents request more via
  `request_config` (human-approved).
- **`agent_extensions` rows** — which extensions are attached (attaching
  never grants authority; grants stay per-tool, per-agent).

See [specs/trust.md](../specs/trust.md) for the containment/authority model
and [specs/schema.md](../specs/schema.md) for the tables.

## Providers

Provider endpoints, models, and priorities are rows in the `providers` and
`models` tables (seeded from `templates/seed.sql`). Agents change them with
the `provider` and `models` sections of a `request_config` changes document
(see [specs/config-doc.md](../specs/config-doc.md)). API keys are stored
encrypted in the `secrets` table. See [specs/providers.md](../specs/providers.md).

## File Layout

```
~/.cclaw/
├── cclaw.db           ← all state (sessions, entries, config, memory, channels)
├── .cclaw_key         ← encryption key for secrets (mode 0600)
├── extensions/        ← shared extension store (promoted bundles)
└── agents/
    └── <name>/
        ├── workspace/ ← the agent's files
        └── skills/    ← optional personal skills
```

Override the DB location with `CCLAW_DB_PATH`; the key, extensions and
agent directories live beside it.

## Quick Start

```bash
export OPENROUTER_API_KEY="sk-or-v1-..."
./build/cclaw
```

Defaults: OpenRouter, DeepSeek V4 Flash, the baseline tool grants listed in
the `agent_default_tools` key — agents request more via `request_config`.
