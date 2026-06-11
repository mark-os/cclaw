# CClaw — Agent Instructions

## Project Ethos

CClaw is a **minimal** autonomous AI agent in C. Every line of code must earn its place.

> *Perfection is achieved, not when there is nothing more to add, but when there is nothing left to take away.* — Antoine de Saint-Exupéry

**Unix Principles**

CClaw is designed around Unix philosophy:
- Daemon as init system — schedules, forks, reaps. Never executes LLM logic.
- Agents as isolated users — each has a workspace directory (`agents/<name>/workspace/`), sessions scoped by agent_name in cclaw.db.
- Processes are cheap and disposable — one turn, then exit. Memory fully reclaimed.
- Communication via exit codes — agents signal intent (0=done, 2=spawn, 3=approval, 4=config), daemon reads details from DB post-reap.
- Config via environment — daemon injects `CCLAW_*` env vars at fork. No config files in agent processes.
- Logging via syslog (daemon) or stderr tee (CLI). No log collector.
- Trust the binary, sandbox the children — agent process is trusted C code; shell/mjs children are untrusted (namespace-sandboxed).

**Inspiration**

CClaw shamelessly borrows ideas from these projects:

| Project | Path | Keep | Leave Out |
|---------|------|------|-----------|
| Pi agent | `reference/pi` | Clean agent loop, session tree model (`ai` and `agent` packages are canonical references) | TypeScript |
| OpenClaw | `reference/openclaw` | Autonomy features, security patterns, multi-channel integration (battle-tested guards for the wild) | Over-engineering, complexity |
| Letta | `reference/letta` | Innovative memory system, stateful agent design | REST API, Postgres, Python |
| nullclaw | `reference/nullclaw` | Zig clone of OpenClaw (architecture reference) | Pure-Zig everything for cross-compat (we link system curl) |
| IronClaw | `reference/ironclaw` | Secure execution model (secret injection, sandboxing) | Rust |

**Principles:**
- Simple over clever. Blocking I/O. Threads over callbacks.
- Single-file SQLite backbone — cclaw.db (all state: sessions, entries, config, memory, channels).
- Self-augmenting via MicroQuickJS plugin system — agents load JS extensions from workspace at startup.
- One tool at a time during development. Prove each layer works before adding the next.
- No backward compatibility. No migrations. No users yet — move fast, break things.

## Target Platforms

- **Primary dev**: EC2 t4g.small (ARM64, Amazon Linux 2023)
- **Secondary dev**: Chromebook (Linux container, ARM64 or x86_64)
- **Production deploy**: any Linux box — small EC2 instances, embedded SoCs, old and new architectures (ARM64, ARMv7, ARMv5TE, x86_64, RISC-V)
- **Known target**: Pogoplug V4 (ARMv5TE, 128MB RAM, Debian Bookworm armel)

**Build/release considerations**: CClaw vendors everything except libcurl (dynamic link to system `libcurl.so`). Multi-arch releases need per-platform builds with the target's cross-compiler + matching libcurl.

## Memory Model

- **Session**: heap-owned growable message array. Each message owns its strings via `malloc`. Only the active session branch is in memory — SQLite holds everything else.
- **Per-turn Arena**: 512KB scratch for LLM request/response JSON, tool output, parsing. Created fresh each turn, destroyed after.
- **Config**: loaded once from env vars at process start. Immutable for process lifetime.
- **AgentContext**: per-turn struct referencing `{session, arena, config}`. Passed to agent/llm functions.

## Security Model

See [specs/security.md](specs/security.md) for full details.

- **Agent process**: trusted binary. `setrlimit` (kernel-enforced) + `http_check_policy()` (app-level).
- **Shell children**: untrusted. Namespace sandbox + transparent credential proxy. See [specs/shell-networking.md](specs/shell-networking.md).
- **Secrets**: encrypted in cclaw.db (ChaCha20-Poly1305). Decrypted by daemon, injected to agent at fork, cleared from env immediately.
- **Secret scanner**: AC-based DLP scans all tool results and user messages for leaked credentials before they enter the context window. See [specs/security.md](specs/security.md#secret-scanner-ac-based-content-dlp).
- **Secret interpolation**: LLMs reference secrets via `{{SECRET:name}}` — cclaw interpolates the real value before tool execution, the context never sees it.
- **Trust levels**: `agents.trust_level` controls shell sandbox strictness (`trusted`, `standard`, `restricted`). See [specs/shell-trust-levels.md](specs/shell-trust-levels.md).

### Choosing a trust_level for new agents

| Level | Use for |
|-------|---------|
| `trusted` | Default agent, bootstrap — full env access, CWD mounted |
| `standard` | Most agents — clean env, network via proxy, workspace rw |
| `restricted` | Observer/audit agents — no network, workspace read-only, tight limits |

### Using secrets in tool calls

Tell the LLM in the system prompt:
> You have these secrets available: `{{SECRET:GITHUB_TOKEN}}`, `{{SECRET:NPM_TOKEN}}`.
> Use `{{SECRET:name}}` in tool arguments. Never write actual secret values.

The `{{SECRET:name}}` syntax works in shell_exec, web_fetch, and js_eval arguments.

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
vendor/        Vendored libs (sqlite3, civetweb, mquickjs, jsmn)
templates/     Schema SQL, system prompts, embedded text (build-time → templates.h)
test/          Test files (test_*.c)
specs/         Detailed reference docs (schema, daemon, memory, providers, security, shell-networking)
reference/     Pi, OpenClaw, nullclaw, Letta clones (gitignored)
build/         Build output (gitignored)
```

## Testing Guidelines

Tests must never hang. Follow these rules:

- **No real network** — unit tests (`make test`) must not connect to real hosts. Use UDS-based mocks or loopback.
- **Timeout on `accept()`** — any mock server thread must set `SO_RCVTIMEO` on the listening socket so it doesn't block forever if the client crashes before connecting.
- **No backward-compatible code** — there are no users yet. No migrations, no deprecation shims, no version checks. Delete old code, don't wrap it.
- **Subprocess tests** — if forking a child that execs something (python, sh), use `waitpid` with awareness that the child may die.
- **Makefile enforces timeouts** — `make test-integration` wraps each binary in `timeout 20`. A hung test is killed after 20s. Output goes to `/tmp/cclaw_<testname>.txt`. `alarm()` only needed in tests with intentional sleeps (retry backoff) — set below 20s. Most integration tests need no alarm.

**Agent workflow for running tests** (prevents shell hangs):

```bash
# Unit tests — fast, safe to run directly
make test

# Integration tests — each binary gets timeout 20s, output captured to file
make test-integration

# Single integration test — redirect to file, read after
./build/test_integration_foo > /tmp/t.txt 2>&1; echo $?
cat /tmp/t.txt            # or: tail -20 /tmp/t.txt

# NEVER pipe a test binary through head/tail/grep directly — it breaks
# timeouts via SIGPIPE. Always redirect to file first, then read the file.

# NEVER pipe make/build output through grep or head either.
# Tests already have timeouts (alarm + Makefile wrapper), output goes to /tmp.
# Just run them directly and check the exit code.
```

## Building

```bash
make              # native build (ARM64 or x86_64)
make test         # unit tests (fast, no network)
make test-integration  # mock-server tests
make test-e2e     # live LLM tests (needs API key)
make clean        # remove build/
```

## Running

```bash
# Minimal — just needs an API key (defaults to OpenRouter + DeepSeek V4 Flash)
export OPENROUTER_API_KEY="sk-or-v1-..."
./build/cclaw              # interactive CLI (default)
./build/cclaw --daemon     # daemon mode (Telegram, web, cron, forks agents)
./build/cclaw --log-level=trace  # full LLM req/resp JSON to stderr
```

Config resolution: `CCLAW_*` env vars > cclaw.db > `OPENROUTER_API_KEY` env.

## Dependencies

| Dep | Purpose | Vendored? |
|-----|---------|-----------|
| jsmn | JSON tokenizer (SSE parsing, tool args) | Yes |
| SQLite 3.53 | Persistence, FTS5, JSON functions | Yes |
| libcurl | HTTP client (LLM API, Telegram) | System (dynamic link) |
| civetweb | Embedded HTTP server (webhooks, dashboard) | Yes |
| MicroQuickJS | JS plugin engine (runtime tool creation, extensions) | Yes |

## LLM Provider

Default: OpenRouter → DeepSeek V4 Flash (`deepseek/deepseek-v4-flash`).

All providers use the OpenAI-compatible chat completions format. Switch provider by setting `provider.base_url` and `provider.api_key` in cclaw.db kv. Env var `OPENROUTER_API_KEY` is all you need to start.
