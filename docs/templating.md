# Templating

CClaw embeds text content at build time from the `templates/` directory. Each file becomes a C string constant, available at runtime without file I/O.

## How It Works

```
templates/*  →  scripts/gen_templates.sh  →  build/templates.h + build/templates.c
```

At build time, `gen_templates.sh` turns every file in `templates/` into a `const char TPL_*[]` constant (declared `extern` in `templates.h`, defined in `templates.c`). The name is the filename in uppercase, with `.` and `-` turned into `_`:

```
templates/default_system_prompt.md  →  TPL_DEFAULT_SYSTEM_PROMPT_MD
templates/schema.sql                →  TPL_SCHEMA_SQL
templates/docs_creating_agents.md   →  TPL_DOCS_CREATING_AGENTS_MD
```

Source files `#include "templates.h"` (from `build/`) and reference the constants directly.

## Template Files

| File | Used by | Purpose |
|------|---------|---------|
| `default_system_prompt.md` | `config.c`, `main.c` | System prompt for the default agent, and the fallback for any agent without its own |
| `schema.sql` | `db.c` | DDL for cclaw.db (unified schema) |
| `seed.sql` | `db.c` | Default provider/model/tool-description rows for a fresh DB |
| `channel_telegram*`, `channel_discord*`, `channel_whatsapp*` | `extension_manifest.c` | Built-in channel extensions (manifest, handler, config), installed into the shared extension store at startup |
| `docs_manifest.json` + `docs_*.md` | `extension_manifest.c` | The built-in `cclaw-docs` extension: each `docs_*.md` is installed as a skill (`skills/<name>/SKILL.md`), so running agents can read their own documentation |
| `cclaw.env.example` / `cclaw.service` / `cclaw.init` | `install.c` | Installed by `cclaw install` (environment file, systemd unit, or SysV init script on non-systemd hosts) |

Every file in `templates/` must have a consumer in `src/` — an embedded but
unused template is dead weight in the binary (this has happened; delete on
sight).

The `docs_*.md` files are read by both people and agents. Keep their YAML
frontmatter (`name:`, `description:`) intact: the description is what an agent
sees in its skill index when deciding whether to open the file.

## Template Variables

Only system prompts support runtime variable substitution:

| Variable | Replaced with |
|----------|---------------|
| `{workspace}` | The agent's workspace path |
| `{session_id}` | Current session ID |
| `{date}` | Current date (YYYY-MM-DD) |
| `{agent_name}` | Agent name (agent prompts only) |

Substitution happens in `agent_build_system_prompt()` (an agent's stored prompt) and `config_render_system_prompt()` (the fallback) at runtime.

All other templates are used verbatim — no variable expansion.

## Customization

Templates are **not configurable at runtime**. To change them, edit the file in `templates/` and rebuild. They represent the agent runtime's built-in behavior, not user preferences.

For user-customizable text, use:
- The agent's system prompt — stored in `agents.system_prompt`; changed through `update_agent` or seeded from `agents/<name>/system.md` (and `agent.json`) the first time an agent of that name is loaded
- `agents/<name>/skills/` — per-agent skills; an index (name + description + path) is injected into the prompt and bodies are loaded on demand (see [specs/skills.md](../specs/skills.md))
- Memory blocks — agent-managed persistent context injected into the prompt

## Adding a New Template

1. Create the file in `templates/`
2. Rebuild — `gen_templates.sh` picks it up automatically
3. Reference it as `TPL_<FILENAME_UPPER>` in C code (include `"templates.h"`)
