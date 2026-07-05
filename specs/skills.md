# Skills — SKILL.md Convention & Progressive Disclosure

cclaw supports the Agent Skills convention (agentskills.io) shared by pi,
OpenClaw, and Claude Code: a skill is a **directory containing `SKILL.md`**
with YAML frontmatter, or a bare `.md` file with the same frontmatter. The
same skill directory works unmodified across all these harnesses — the only
cross-tool difference in the ecosystem is discovery location, not format.

Implementation: `src/skills.c` (discovery + frontmatter + index),
`src/agent_config.c` (`agent_build_system_prompt` injects the index),
`src/main.c` file-tier dispatch (mounts skill dirs read-only).

## Format

```
my-skill/
  SKILL.md        required — frontmatter + instructions body
  scripts/        optional supporting files (read on demand)
  references/
```

```yaml
---
name: my-skill            # optional — falls back to the directory/file name
description: Does X when Y   # REQUIRED — this is the trigger text
---
# Instructions body — loaded only on demand
```

Parsing is deliberately minimal (`skill_frontmatter_parse`): top-level
`name:` and `description:` from the frontmatter block; everything else
(including OpenClaw's `metadata.openclaw.*` gating/install blocks) is
ignored gracefully. A skill with no `description` is skipped — the
description *is* the index entry, so it is useless without one.

## Progressive disclosure — why skills live on the filesystem

The system prompt receives only an index (same XML shape as pi/OpenClaw):

```xml
## Skills
The following skills are available. When a task matches a skill's
description, use file_read on its location to load the full instructions...
<available_skills>
  <skill>
    <name>weather</name>
    <description>Current weather and forecasts</description>
    <location>/home/u/.cclaw/skills/weather/SKILL.md</location>
  </skill>
</available_skills>
```

The body is **never** injected up front — the model `file_read`s the
location when a task matches. This replaced the old model (concatenate every
`agents/<name>/skills/*.md` wholesale into the prompt), which taxed every
turn with every skill. It also forces the storage decision: skill bodies
must be real files at stable paths the file-tool sandbox can read — a
DB-stored body would break both lazy loading and drop-in ecosystem compat.
The DB stores nothing about standalone skills; discovery rescans at prompt
build (a few `readdir`s).

To make locations readable, the file-tier dispatch (`src/main.c`) appends
the resolved skill dirs to the child's read-only mounts — the same transient
override pattern the JS tier uses for the extension store.

## Discovery

1. Each entry of the `skills_dirs` config key (JSON array, `~` expanded;
   registry default `["~/.cclaw/skills", "~/.agents/skills"]`) — operator
   knob, visible in `search_config`, overridable via the config registry.
   Point it at `~/.claude/skills` and existing Claude Code skills appear.
2. The per-agent dir `agents/<name>/skills/` — agent-personal skills, the
   self-augmentation surface (an agent writes its own skill here).

Within a dir: subdirectories containing `SKILL.md`, plus bare `*.md` files
(filename = fallback name). **First name wins** on collision, so configured
dirs shadow the per-agent dir.

## Trust notes

- Skill descriptions and bodies are prompt content from disk. Agent-authored
  skills in the agent's own tree are self-authored context (same trust class
  as memory). The global dirs are operator-managed by definition.
- Future (extensions as bundles): an extension manifest may ship `skills/`
  dirs; promotion places them under the extension store and enumeration at
  promote-approval must list them ("includes N skills — modifies system
  prompt") alongside tools/hooks. Standalone and packaged skills are the
  same artifact, differing only in discovery root.
- Deliberately not implemented: OpenClaw's eligibility gating
  (`requires.bins/env/os`) and install specs. The sandbox makes host-binary
  checks misleading (present on host ≠ present in the child namespace);
  revisit if it earns its place.
