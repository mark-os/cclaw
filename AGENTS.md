# CClaw — Agent Instructions

## Project Ethos

CClaw is a **minimal** autonomous AI agent in C. Every line of code must earn its place.

**Inspiration**

CClaw draws from *Pi agent* (`reference/pi`) for its clean agent loop and session tree model, and from *OpenClaw* (`reference/openclaw`) for autonomy features, security patterns, and multi-channel integration. It also learns from *nullclaw* (`reference/nullclaw`), a Zig clone of OpenClaw. CClaw does not try to be everything to everyone — it serves its creator Mark Ostroth. It will borrow C libraries, link dynamically to system curl, vendor what makes sense, and do whatever it takes to produce a simple, usable, excellent autonomous agent.

**Principles:**
- Simple over clever. Blocking I/O. Threads over callbacks. No event loops.
- SQLite is the backbone — sessions, history, search, sub-agent coordination.
- Self-modifying via MicroQuickJS — agent can define new tools at runtime.
- One tool at a time during development. Prove each layer works before adding the next.
- Reference Pi for agent loop patterns. Reference OpenClaw for integration and autonomy.

## Target Platforms

- **Primary dev**: EC2 t4g.small (ARM64, Amazon Linux 2023)
- **Secondary dev**: Chromebook (Linux container, ARM64 or x86_64)
- **Eventual deploy target**: Pogoplug V4 (ARMv5TE, 128MB RAM, Debian Bookworm armel) — cross-compile when ready

## Memory Model

- **Session**: heap-owned growable message array. Each message owns its strings via `malloc`. Only the active session branch is in memory — SQLite holds everything else.
- **Per-turn Arena**: 512KB scratch for LLM request/response JSON, tool output, parsing. Created fresh each turn, destroyed after.
- **Config Arena**: small (4KB), process lifetime, read-only after load.
- **AgentContext**: per-turn struct referencing `{session, arena, config}`. Passed to agent/llm functions.

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
vendor/        Vendored libs (cJSON, sqlite3, civetweb, mquickjs)
test/          Test files (test_*.c)
notes/         Research & scoping notes (kept during development)
reference/     Pi, OpenClaw clones (gitignored)
build/         Build output (gitignored)
```

## Building

```bash
make              # native build (ARM64 or x86_64)
make test         # run tests
make clean        # remove build/
```

## Running

```bash
# Minimal — just needs an API key (defaults to OpenRouter + DeepSeek V4 Flash)
export OPENROUTER_API_KEY="sk-or-v1-..."
./build/cclaw

# With config file
./build/cclaw config.json

# CLI mode (no daemon, stdin/stdout)
./build/cclaw --cli

# Multiple instances can share the same SQLite DB (WAL mode)
```

## Dependencies

| Dep | Purpose | Vendored? |
|-----|---------|-----------|
| cJSON | JSON parsing | Yes |
| SQLite 3.53 | Persistence, FTS5, JSON functions | Yes |
| libcurl | HTTP client (LLM API, Telegram, WhatsApp) | System (dynamic link) |
| civetweb | Embedded HTTP server (webhooks, dashboard) | Yes |
| MicroQuickJS | JS scripting engine (runtime tool creation) | Yes |

## LLM Provider

Default: OpenRouter → DeepSeek V4 Flash (`deepseek/deepseek-v4-flash`).

All providers use the OpenAI-compatible chat completions format. Switch provider by changing `base_url` and `api_key` in config. Env var `OPENROUTER_API_KEY` is all you need to start.

