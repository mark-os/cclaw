# CClaw — Agent Instructions

## Project Ethos

CClaw is a **minimal** autonomous AI agent in C. Every line of code must earn its place.

> *Perfection is achieved, not when there is nothing more to add, but when there is nothing left to take away.* — Antoine de Saint-Exupéry

**Unix Principles**

CClaw is designed around Unix philosophy:
- Daemon as supervisor — schedules work, manages channels, dispatches to worker threads. Tool children are forked for sandbox isolation.
- Agents as isolated users — each has a workspace directory (`agents/<name>/workspace/`), sessions scoped by agent_name in cclaw.db.
- Long-lived core, disposable work — the daemon process stays up running the event loop. The cheap, reclaimable unit is the forked tool child (exits after one tool call). No per-turn process churn — LLM requests are blocking curl calls on a worker thread pool that write results back to the DB.
- Communication via DB state — `advance_session()` reads session state, decides next action. No IPC beyond worker notification pipe.
- Config via environment — process reads `CCLAW_*` env vars at startup. No config files in agent processes.
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
| Hermes | `reference/hermes` | Self-improving agent loop, skill/extension library patterns (closest cousin to our self-augmentation goal), multi-channel gateway | TS/Python runtime sprawl, huge feature surface |

**Principles:**
- Simple over clever. Blocking I/O. Threads over callbacks.
- Single-file SQLite backbone — cclaw.db (all state: sessions, entries, config, memory, channels).
- Self-augmenting via MicroQuickJS plugin system — agents load JS extensions from workspace at startup.
- One tool at a time during development. Prove each layer works before adding the next.
- No backward compatibility. No migrations. No users yet — move fast, break things.

## Working With the Grain

CClaw follows the **principle of least surprise**: pick the boring, obvious solution a maintainer would guess, match the patterns already in the file, and get the job done. The cleverness is in *choosing the right existing tool* — SQLite and Unix — not in writing new machinery. Before adding a data structure, cache, queue, or state machine in C, ask whether SQLite or the OS already does it.

**Lean on SQLite — it is the architecture, not just storage.**
- Build and parse structured JSON with SQLite's JSON1 (`json_object`, `json_group_array`, `json_each`, `json_patch`), not hand-rolled C. The LLM request body is assembled this way (`src/llm_payload.c`) — a SQL query over `entries` *is* the serializer.
- Search with FTS5. Queues and work state are tables. Concurrency is WAL. Ordering is `ORDER BY pos`. Reach for a C container only when SQLite genuinely can't express it.

**Trust the Unix system — don't reimplement it in C.**
- Isolation: namespaces, `setrlimit`, file permissions, `fork`/`exec`. Lifecycle: signals + the event loop. Scheduling: cron. Communication: fds and the worker pipe. Let the kernel do the kernel's job.

**State has one home.** Durable state is `cclaw.db`. Memory holds only the active session branch and per-turn scratch. Do **not** introduce parallel state (globals, caches, sidecar files) that can drift from the DB — state management is the part that churned the most before stabilizing, so changes here have wide blast radius. `advance_session()` is the load-bearing wall: re-read it before changing how a turn progresses.

**jsmn vs SQLite JSON — use the right one.** jsmn tokenizes *untrusted/streaming* JSON you don't own (SSE chunks, tool-call arguments from the model). SQLite JSON handles *structured data you own* in the DB. They are not interchangeable; don't swap one for the other to "unify."

### Counterintuitive on purpose — don't "fix" these

These look odd at a glance but are deliberate. Understand them before touching them; if you think one is wrong, that's the signal to ask, not to refactor.

- **A SQL query emits the LLM request JSON.** `src/llm_payload.c` returns the request body zero-copy from an open statement. This is intentional and fast — do not replace it with a C JSON builder.
- **Forked tool children, threaded LLM calls.** LLM requests run on a worker thread pool in the long-lived process; only untrusted/blocking tools fork. (This replaced an earlier fork-per-turn design — don't reintroduce it.)
- **No config files in agent logic, no migrations, no compat shims.** Config comes from env at startup; there are no users, so delete old code instead of versioning it.

## Target Platforms

- **Primary dev**: EC2 t4g.small (ARM64, Amazon Linux 2023)
- **Secondary dev**: Chromebook (Linux container, ARM64 or x86_64)
- **Production deploy**: any Linux box — small EC2 instances, embedded SoCs, old and new architectures (ARM64, ARMv7, ARMv5TE, x86_64, RISC-V)
- **Known target**: Pogoplug V4 (ARMv5TE, 128MB RAM, Debian Bookworm armel)

**Build/release considerations**: CClaw vendors everything except libcurl (dynamic link to system `libcurl.so`). Multi-arch releases need per-platform builds with the target's cross-compiler + matching libcurl.

## Memory Model

- **Session**: heap-owned growable message array. Each message owns its strings via `malloc`. Only the active session branch is in memory — SQLite holds everything else.
- **Config**: loaded once from env vars at process start. Immutable for process lifetime.
- **AgentContext**: per-turn struct referencing `{session, config}`. Passed to agent/llm functions.

## Self-Augmentation (core differentiator)

This is what CClaw *is for*, not an add-on. Agents extend themselves at runtime via the MicroQuickJS engine, adding **new tools, channels, and scripts**.

**One model: JS lives in files, the DB holds config + a path.** Every JS artifact — tools, channels, scripts — is a file in the agent's workspace (`agents/<name>/workspace/`), referenced by path. The DB stores the *definition* (name, description, JSON schema, trust flags) and a `path` to the implementation; it never stores code. The Telegram channel is the canonical example: `channels.extension_name` joins `extensions.name` to resolve a `js_path`, and the runner loads that file (`src/channel_runner.c`).

- **Definition is config, not code.** `js_define_tool` is a config change: the agent passes a **path to a code file** in its workspace plus the tool's schema — never inline JS. Defining a tool = write/point at the draft file + insert the `extensions` (and `agent_extensions`) row.
- **Draft → promote lifecycle.** JS starts as a draft file in `workspace/extensions/`, usable only by that agent. Promotion registers it (`name → path` in `extensions`, linked via `agent_extensions`) so it loads at startup — and that registration is the trust boundary (a sub-agent's draft must not auto-promote to a global tool).
- `js_eval` runs JS in the sandboxed engine for one-off evaluation.
- The JS bridge (tool registration, channel dispatch) is a first-class API surface. When changing how tools register or channels dispatch, preserve the agent's ability to self-augment — don't optimize it away as "just plugins."

> **Slated for removal:** the legacy code-in-DB path — the `js_tools.code` column and its startup reload (`SELECT ... code FROM js_tools`, `src/tool_js.c`). It is the lone artifact that stores JS in the DB; fold `js_define_tool` onto the path-based `extensions` model above and delete it.

## Security Model

See [specs/security.md](specs/security.md) for full details.

- **Agent process**: trusted binary. `setrlimit` (kernel-enforced) + `http_check_policy()` (app-level).
- **Shell children**: untrusted. Namespace sandbox + transparent credential proxy. See [specs/shell-networking.md](specs/shell-networking.md).
- **Secrets**: encrypted in cclaw.db (ChaCha20-Poly1305). Decrypted at runtime, injected to tool children via env, never exposed to the model or logged.
- **Secret scanner**: AC-based DLP scans all tool results and user messages for leaked credentials before they enter the context window. See [specs/security.md](specs/security.md#secret-scanner-ac-based-content-dlp).
- **Secret interpolation**: LLMs reference secrets via `{{SECRET:name}}` — cclaw interpolates the real value before tool execution, the context never sees it.
- **Trust levels**: `agents.trust_level` controls shell sandbox strictness (`host`, `trusted`, `standard`, `restricted`). Every level except `host` *requires* the namespace sandbox — if it can't be established, the shell refuses to run (fail-closed). See [specs/shell-trust-levels.md](specs/shell-trust-levels.md).

### Choosing a trust_level for new agents

| Level | Use for |
|-------|---------|
| `host` | Dev / hosts without unprivileged userns — **no sandbox at all** (`-y` forces this) |
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
- **Makefile enforces timeouts** — `make test` wraps each binary in `timeout 20`, `make test-integration` in `timeout 45`. A hung test is killed. Per-test output goes to `/tmp/cclaw_<testname>.txt`. `alarm()` only needed in tests with intentional sleeps (retry backoff) — set below the wrapper timeout. Most tests need no alarm.
- **Line-buffer test stdout** — `setvbuf(stdout, NULL, _IOLBF, 0)` first thing in main. When the timeout wrapper kills a hung test, fully-buffered stdout vanishes and the /tmp log shows only stderr — you debug blind. (Running a binary manually, `stdbuf -oL -eL` does the same.)
- **Clean ALL derived artifacts at startup, not just the .db** — a killed run leaves `<db>-wal`, `<db>-shm`, `<db_base>.<channel>.pipe`, `.sock`, and possibly an orphaned child process holding them. Stale debris makes the next run hang somewhere different, so repros look non-deterministic. Integration tests should start by removing the whole `<db_base>*` family.
- **Isolate before instrumenting** — when an integration test hangs, reproduce the single suspect path against a minimal standalone harness (a 20-line python HTTP server, a bare UDS client) before adding debug output to the full test. The full test couples DB + mock server + FIFO + child lifecycle; the bug is usually visible in one of them alone.

**Agent workflow for running tests**:

```bash
# Both targets are pipe-safe: each binary's output is captured to
# /tmp/cclaw_<testname>.txt and make prints one PASS/FAIL line per suite
# (plus the tail of the log on failure). `make test | tail -20` is fine.
make test               # unit tests
make test-integration   # mock-server tests

# Single test binary — redirect to file, read after
./build/test_foo > /tmp/t.txt 2>&1; echo $?
cat /tmp/t.txt            # or: tail -20 /tmp/t.txt

# NEVER pipe an individual test binary through head/tail/grep — a forked
# mock-server child can hold the pipe open (reader hangs waiting for EOF),
# and an early-exit reader SIGPIPEs the test mid-run. The make targets are
# immune because children inherit a /tmp file fd, never your pipe.
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
./build/cclaw --daemon     # daemon mode (Telegram, web, cron, multi-agent)
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
