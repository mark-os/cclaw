# Templating

CClaw embeds text content at build time via the `templates/` directory. Each file becomes a C string constant available at runtime without file I/O.

## How It Works

```
templates/*.{md,txt,sql}  →  scripts/gen_templates.sh  →  build/templates.h
```

At build time, `gen_templates.sh` converts each file into a `static const char TPL_*[]` constant. The naming convention:

```
templates/default_system_prompt.md  →  TPL_DEFAULT_SYSTEM_PROMPT_MD
templates/schema.sql                →  TPL_SCHEMA_SQL
templates/seed.sql                  →  TPL_SEED_SQL
```

Source files `#include "templates.h"` (from `build/`) and reference the constants directly.

## Template Files

| File | Used by | Purpose |
|------|---------|---------|
| `default_system_prompt.md` | `config.c` | Default system prompt when no agent-specific prompt exists |
| `schema.sql` | `db.c` | DDL for cclaw.db (unified schema) |
| `seed.sql` | `db.c` | Default provider/model/tool-description rows for a fresh DB |
| `channel_telegram.json` / `channel_telegram.qjs` | `main.c` | Telegram channel extension written out by `configure_channel` |
| `cclaw.env.example` / `cclaw.service` | `install.c` | Installed alongside the binary for daemon deployment |

Every file in `templates/` must have a consumer in `src/` — an embedded-but-
unused template is dead weight in the binary (this has happened; delete on
sight).

## Template Variables

Only `default_system_prompt.md` supports runtime variable substitution:

| Variable | Replaced with |
|----------|---------------|
| `{workspace}` | Agent's workspace path |
| `{session_id}` | Current session ID |
| `{date}` | Current date (YYYY-MM-DD) |
| `{agent_name}` | Agent name (when using agent-specific prompts) |

Substitution happens in `config_render_system_prompt()` and `agent_build_system_prompt()` at runtime.

All other templates are used verbatim — no variable expansion.

## Customization

Templates are **not configurable at runtime**. To change them, edit the file in `templates/` and rebuild. They represent the agent runtime's built-in behavior, not user preferences.

For user-customizable text, use:
- `agents/<name>/system.md` — per-agent system prompt (overrides the default template entirely)
- `agents/<name>/skills/` — per-agent skills; an index (name + description + path) is injected into the prompt and bodies are loaded on demand (see specs/skills.md)
- Memory blocks — agent-managed persistent context injected into the prompt

## Adding a New Template

1. Create the file in `templates/` (any `.md`, `.txt`, or `.sql` extension)
2. Rebuild — `gen_templates.sh` picks it up automatically
3. Reference as `TPL_<FILENAME_UPPER>` in C code (include `"templates.h"`)
