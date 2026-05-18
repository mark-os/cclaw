<!-- br-agent-instructions-v1 -->

# CClaw — Agent Instructions

## Project Ethos

CClaw is a **minimal** AI agent in C. The goal is the smallest correct implementation that works on constrained hardware (ARMv5TE Pogoplug with 128MB RAM). Every line of code must earn its place.

**Principles:**
- Simple over clever. Blocking I/O over event loops. Threads over callbacks.
- Test-driven: write the test first, then the implementation.
- Reference Pi for agent loop patterns (clean, battle-tested TypeScript).
- Reference nullclaw for deployment patterns (Zig, same Pogoplug target).
- Reference OpenClaw for Telegram integration patterns.
- One tool at a time. `shell_exec` first, prove the loop works, then add more.
- Arena allocators for per-turn memory. No leak hunting.
- Static linking for deployment. Single binary, scp to device, done.

## Code Style

- C11, `-Wall -Wextra -Werror`
- 4-space indent, no tabs
- Snake_case for functions and variables
- UPPER_CASE for constants and macros
- Structs: `typedef struct { ... } TypeName;` (PascalCase)
- Headers: include guard `#ifndef CCLAW_MODULE_H`
- Keep functions short. If it doesn't fit on a screen, split it.
- Comments explain *why*, not *what*.

## File Layout

```
src/           C source files
include/       C headers (public API for each module)
vendor/        Vendored libs (cJSON, sqlite3)
test/          Test files (test_*.c)
reference/     Pi, OpenClaw, nullclaw clones (gitignored)
build/         Build output (gitignored)
docs/          Design docs
```

## Beads Workflow

This project uses [beads_rust](https://github.com/Dicklesworthstone/beads_rust) (`br`) for issue tracking.

```bash
br ready              # What's next (open, unblocked)
br show <id>          # Full details + dependencies
br list --status=open # All open work
br create --title="..." --description="..." --type=task --priority=2
br update <id> --status=in_progress
br close <id> --reason="Done"
br sync --flush-only  # Export to JSONL for git
```

**Workflow**: `br ready` → claim → implement (TDD) → `br close` → `br sync --flush-only` → commit

<!-- end-br-agent-instructions -->
