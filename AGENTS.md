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
- Trust the binary, sandbox the children — agent process is trusted C code; shell/qjs children are untrusted (namespace-sandboxed).

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
- Self-augmenting via QuickJS plugin system — agents load JS extensions from workspace at startup.
- One tool at a time during development. Prove each layer works before adding the next.
- No backward compatibility in code — delete old code, don't version it. DB schema changes are the one exception: they ship as forward-only patches (see Running).

## Working With the Grain

CClaw follows the **principle of least surprise**: pick the boring, obvious solution a maintainer would guess, match the patterns already in the file, and get the job done. The cleverness is in *choosing the right existing tool* — SQLite and Unix — not in writing new machinery. Before adding a data structure, cache, queue, or state machine in C, ask whether SQLite or the OS already does it.

**Lean on SQLite — it is the architecture, not just storage.**
- Build and parse structured JSON with SQLite's JSON1 (`json_object`, `json_group_array`, `json_each`, `json_patch`), not hand-rolled C. The LLM request body is assembled this way (`src/llm_payload.c`) — a SQL query over `entries` *is* the serializer.
- Search with FTS5. Queues and work state are tables. Concurrency is WAL. Ordering is `ORDER BY pos`. Reach for a C container only when SQLite genuinely can't express it.

**Trust the Unix system — don't reimplement it in C.**
- Isolation: namespaces, `setrlimit`, file permissions, `fork`/`exec`. Lifecycle: signals + the event loop. Scheduling: cron. Communication: fds and the worker pipe. Let the kernel do the kernel's job.

**State has one home.** Durable state is `cclaw.db`. Memory holds only the active session branch and per-turn scratch. Do **not** introduce parallel state (globals, caches, sidecar files) that can drift from the DB — state management is the part that churned the most before stabilizing, so changes here have wide blast radius. `advance_session()` is the load-bearing wall: re-read it before changing how a turn progresses.

**jsmn vs SQLite JSON — use the right one.** SQLite JSON1 is the parser everywhere a db handle exists — including model-generated tool-call arguments, which the trusted parent validates and decomposes once at dispatch (`tool_args.c`); sandboxed `--run-tool` children receive pre-extracted params over the flat wire and parse no JSON at all. jsmn survives only where genuinely no db is in reach (the channel-harness scenario reader). Don't reintroduce a second JSON parser on a path that has a db handle.

### Counterintuitive on purpose — don't "fix" these

These look odd at a glance but are deliberate. Understand them before touching them; if you think one is wrong, that's the signal to ask, not to refactor.

- **A SQL query emits the LLM request JSON.** `src/llm_payload.c` returns the request body zero-copy from an open statement. This is intentional and fast — do not replace it with a C JSON builder.
- **Forked tool children, threaded LLM calls.** LLM requests run on a worker thread pool in the long-lived process; only untrusted/blocking tools fork. (This replaced an earlier fork-per-turn design — don't reintroduce it.)
- **No config files in agent logic, no compat shims.** Config comes from env at startup; delete old code instead of versioning it. The one sanctioned versioning mechanism is the DB schema patch list (`schema_patches[]` in `src/db.c`).
- **One binary for every process mode — no per-subsystem "thin" child.** The re-exec'd `cclaw --run-tool` broker (all sandboxed tool tiers, including JS) and the `cclaw --channel` runner reuse the full image. COW fork + demand paging already keep the subsystems a child never calls (civetweb, most of SQLite) out of its resident set; `--run-tool` is intercepted at the top of `main()` before any DB/config/key init, so the child never even starts them. Splitting out a minimal child binary would buy nothing and is not worth the build/maintenance cost.

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

## Turn Model Terminology

Precise names for the execution units — use these consistently in code,
comments, and specs (the schema misnames one; see below):

- **Turn** — inbox consumption → final assistant message + delivery. The unit
  users/channels see, bounded by `max_iterations`; the boundary where
  `notify_parent` and compaction run.
- **Iteration** — one LLM request → one assistant response → that response's
  tool calls fully resolved (the turn-join). Counted by
  `sessions.turn_iteration`.
- **Batch** — the tool calls of one assistant response, claimed in call order:
  inline → immediate result; serial async → blocks the rest of the batch;
  parallel-safe → dispatched concurrently.

**The mid-turn invariant**: only two kinds of writes reach `entries` mid-turn —
LLM output, and tool results answering this turn's calls. Everything else
(channel messages, cron fires, sub-agent/background results) queues in the
inbox and enters at a turn boundary as user entries. This one rule buys
provider adjacency legality, a byte-stable prefix for prompt caching, and turn
atomicity — don't break it.

**Freshness tiers**: the session context block is *turn-fresh* (frozen at
iteration 1, `session_set_turn_context`); the head (system prompt, tools
array, launch_agent's embedded roster) is *iteration-fresh* (rebuilt every
request, byte-stable in practice); pull surfaces (`check_session`) are *live*.

**Known misnomer**: `entries.turn_id` / `llm_responses.turn_id` are minted per
LLM request (`db_next_turn_id`) — they identify an *iteration*, not a turn.

## Self-Augmentation (core differentiator)

This is what CClaw *is for*, not an add-on. Agents extend themselves at runtime via the QuickJS engine, adding **new tools, channels, and scripts**.

**One model: JS lives in files, the DB holds config + a path.** Every JS artifact — tools, channels, scripts — is a file in the agent's workspace (`agents/<name>/workspace/`), referenced by path. The DB stores the *definition* (name, description, JSON schema, trust flags) and a `path` to the implementation; it never stores code. The Telegram channel is the canonical example: `channels.extension_name` joins `extensions.name` to resolve a `js_path`, and the runner loads that file (`src/channel_runner.c`).

- **Definition is config, not code.** A tool is declared in an extension manifest (`extension.json`): name, description, JSON schema, and a **path to a handler file** — never inline JS. Defining a tool = write the draft bundle + `extension_promote` (which validates the manifest and ingests the `extensions`/`tools`/`agent_extensions` rows).
- **Draft → promote lifecycle.** JS starts as a draft file in `workspace/extensions/`, usable only by that agent. Promotion registers it (`name → path` in `extensions`, linked via `agent_extensions`) so it loads at startup — and that registration is the trust boundary (a sub-agent's draft must not auto-promote to a global tool).
- `js_eval` runs JS in the sandboxed engine for one-off evaluation.
- The JS bridge (tool registration, channel dispatch) is a first-class API surface. When changing how tools register or channels dispatch, preserve the agent's ability to self-augment — don't optimize it away as "just plugins."

## Security Model

See [specs/security.md](specs/security.md) for full details and [specs/trust.md](specs/trust.md) for the axis model (containment / authority / escalation / sensitivity).

- **Agent process**: trusted binary. `setrlimit` (kernel-enforced); tool egress enforced at the credential proxy (`host_decide()`, default-deny).
- **Shell children**: untrusted. Namespace sandbox + transparent credential proxy. See [specs/shell-networking.md](specs/shell-networking.md).
- **Secrets**: encrypted in cclaw.db (ChaCha20-Poly1305). Decrypted at runtime, injected to tool children via env, never exposed to the model or logged.
- **Secret scanner**: AC-based DLP scans all tool results and user messages for leaked credentials before they enter the context window. See [specs/security.md](specs/security.md#secret-scanner-ac-based-content-dlp).
- **Secret interpolation**: LLMs reference secrets via `{{SECRET:name}}` — cclaw interpolates the real value before tool execution, the context never sees it.
- **Sandbox profiles**: `agents.sandbox_profile` controls containment only (`host`, `standard`, `restricted`) — authority always lives in `grants`, never on the profile. Every profile except `host` *requires* the namespace sandbox — if it can't be established, the shell refuses to run (fail-closed). See [specs/sandbox-profiles.md](specs/sandbox-profiles.md).

### Choosing a sandbox_profile for new agents

| Profile | Use for |
|-------|---------|
| `host` | **No sandbox at all** — skips *both* the namespace and the egress proxy (traffic goes direct, unfiltered). Use when the surrounding environment already provides isolation (inside a Docker container, behind a firewall) or on hosts where unprivileged userns is unavailable. `--trust-host` forces this. |
| `standard` | **The default.** Clean env, network via proxy, workspace rw, no CWD mount — it cannot see the user's files or another agent's workspace without a grant. NPROC 256 as a fork-bomb backstop; no CPU cap. |
| `restricted` | Observer/audit agents — **no packets, ever** (kernel-enforced: no proxy socket in the child); still able to run real programs (workspace rw, bounded NPROC/CPU). File grants remain legal. |

Widening what an agent can see is the `grants` + approval path, never a side
effect of a looser profile. `cclaw.db` and `.cclaw_key` are masked
unconditionally under every sandboxed profile and a path grant naming either
(or a directory containing them) is refused at grant time — `db_query` is the
sanctioned way for an agent to inspect state. `$HOME` is set under
every profile (the agent workspace when sandboxed, the real user home under
`host`), `PATH` includes both system and workspace-local bin dirs, and `/tmp` is
a bind of a persistent per-agent scratch dir on the host (so it inherits the
host's own temp storage and cleaner, rather than a size policy of ours) — so
`npm install`, `cargo build`, and `go build` work inside the sandbox. No profile sets `RLIMIT_AS`; see
[specs/sandbox-profiles.md](specs/sandbox-profiles.md) for why.

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
src/           C source files + headers (each module's .c and .h live together)
vendor/        Vendored libs (sqlite3, civetweb, quickjs, jsmn)
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
- **No backward-compatible code** — no deprecation shims, no version checks. Delete old code, don't wrap it. (DB schema changes are the exception: they need a forward patch in `schema_patches[]`, not a shim — see Running.)
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
make test-asan          # unit tests under ASan/UBSan — heavy; only when explicitly asked

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
make debug        # clean + clang build with -O0 -g3 + ASan/UBSan
make smoke        # curated fast unit subset (~10 suites, a few seconds)
make test         # unit tests (fast, no network)
make test-integration  # mock-server tests
make test-e2e     # live LLM tests (needs API key)
make clean        # remove build/
```

**Escalate, don't default to the top.** `make smoke` (~8s) is 10x faster than `make test` (~90s) because it's a *smaller test set*, not a build-cache effect — `test`'s time is almost all execution, not compilation, so there's no free lunch by rerunning it.

- While actively editing: `make smoke` after each change.
- Before a commit: `make test` — cheap enough to run every time.
- Before a commit that touches channels, LLM proc, retries, or shell proxy (mock-server-exercised paths): also `make test-integration` (~66s). Skip it for changes clearly orthogonal to those areas.
- `make test-asan` (full suite under ASan/UBSan, ~200s clean-instrumented rebuild): never unprompted — only when explicitly asked.

- **`make debug` uses clang, not gcc.** GCC-only `#pragma GCC diagnostic` directives must be guarded with `#if defined(__GNUC__) && !defined(__clang__)` — under `-Werror` clang turns an unknown warning group (e.g. `-Wformat-truncation`) into a hard error.
- **`make test` builds `build/cclaw` first.** Several unit tests (`test_run_tool_escape`, `test_sandbox_mounts`, `test_shell_namespace`, `test_shell_secrets`, `test_workspace_isolation`) fork the main binary via the `--run-tool` path, so the `test` target depends on `$(BUILDDIR)/cclaw` — it's always rebuilt before the suite runs, even after `make clean`. (`make smoke` deliberately omits the dep: its curated subset has no fork-the-binary tests.)
- **`build/` self-cleans on a mode switch.** A `.buildtag` sentinel (checked at parse time) wipes `build/` automatically when `CC`/`EXTRA_CFLAGS` change between invocations — e.g. a `make debug` followed by a plain `make test` no longer link-fails on undefined `__asan_*`/`__ubsan_*` symbols from stale instrumented objects; it just rebuilds clean.
- **Sanitizer builds skip `RLIMIT_AS`** (`src/sandbox.c`, compile-time guard): ASan reserves terabytes of shadow VA and its allocator mmaps at runtime, so an address-space cap aborts the instrumented child with "Failed to mmap". Release builds enforce the cap unchanged.

## Running

```bash
# Minimal — just needs an API key (defaults to OpenRouter + DeepSeek V4 Flash)
export OPENROUTER_API_KEY="sk-or-v1-..."
./build/cclaw              # interactive CLI (default)
./build/cclaw -p "..."     # single-turn: send prompt, print response, exit
./build/cclaw -p "..." -s <id>   # single-turn against an existing session
./build/cclaw --daemon     # daemon mode (Telegram, web, cron, multi-agent)
./build/cclaw --log-level=trace  # full LLM req/resp JSON to stderr
```

- **The real DB is `~/.cclaw/cclaw.db`, not `./cclaw.db`.** `resolve_db_path()` returns `$HOME/.cclaw/cclaw.db` when `$HOME` is set (override with `CCLAW_DB_PATH`). "Delete the db" means that path — and its `-wal`/`-shm` siblings.
- **Schema changes need a forward patch.** When `templates/schema.sql` changes shape, bump `CCLAW_SCHEMA_VERSION` (`src/cclaw.h`) and append a matching entry to `schema_patches[]` (`src/db.c`) that brings a live DB from the previous version to the new one. Startup auto-applies pending patches; DBs newer than the build, or older than the floor `CCLAW_SCHEMA_MIN` (v33 — the 2026-07-19 freeze collapsed prior patch history into it), are refused with a delete-and-restart message. A schema.sql change *without* the bump + patch leaves existing DBs stamped current but shaped old — missing columns, `advance_session` returns `ADVANCE_ERROR`, and the CLI can hang.
- **`-p` with piped/non-tty stdin auto-selects the most recent session**; `-s <id>` pins one. Useful for scripted multi-turn testing (turn 1 creates the session, reuse its id for turn 2+).

### Scripted agent testing with `-p` (what to expect)

- **Approvals park and expire between `-p` runs.** Single-turn mode has no
  interactive approver: a `request_config` call parks, the process exits, and
  the next run tells the agent "Approval #N expired without a decision" — the
  agent then tends to loop re-requesting. Plan multi-turn scripts around this.
- **`--auto-approve` approves whatever is parked, not what you meant.** It is
  single-use and blind to which approval it answers — inspect
  `select id, tool_call_id, args_json, state from approvals` before and after.
- **Verify config changes in the DB, never from the agent's claims.** The
  agent will assert success ("Gemini is now active") without checking; the
  truth is `agents.primary_model`, `grants`, `approvals.state`.
- **Debugging trail for a turn:** `entries` (role 2=assistant tool-call args,
  3=tool results, 1 includes system approval notices), `tool_calls`
  (pending/done per call_id), `approvals` (parked docs), `cclaw resp` (LLM
  wire traffic). Approval args_json should match the tool call that parked it
  — if they diverge, you're looking at a parking bug, not agent confusion.

Config resolution: `CCLAW_*` env vars > cclaw.db > `OPENROUTER_API_KEY` env.

### Debugging a failed turn

- **Follow the citation.** Error entries end with `[resp #N]` — read that row with
  `cclaw resp` (bare = most recent failure; `resp <id> [req]` for one row or the
  request we sent; `resp list [n]`).
- **Never SELECT `llm_responses.body` with a system sqlite3 older than 3.45** — it's
  JSONB and dumps as binary garbage (that's the format, not corruption). `cclaw resp`
  reads it with the vendored SQLite. Metadata columns (`status`, `model`, `turn_id`,
  `length(body)`) are safe to query anywhere.
- **Verbosity**: `-v` (debug: timing, retry decisions) / `-vv` (trace: full req/resp
  JSON), or `--log-level=LEVEL`.
- **Policy reference**: [specs/error-handling.md](specs/error-handling.md) — failure
  taxonomy E1–E14, retry/backoff/fallback, model degradation. The session's DB trail
  is `entries` (roles 0=system 1=user 2=assistant 3=tool 4=compaction); a stuck
  session shows a non-idle `sessions.state` with a dead `owner_instance`.

> **Workspace must always be set by `config_load()`.** If `cfg->workspace` is left NULL, file tools, the proxy mount, and `workspace_init()` all fail with "no workspace configured" even though the env path looks fine.

## Dependencies

| Dep | Purpose | Vendored? |
|-----|---------|-----------|
| jsmn | JSON tokenizer (db-less corners, e.g. channel-harness scenarios) | Yes |
| SQLite 3.53 | Persistence, FTS5, JSON functions | Yes |
| libcurl | HTTP client (LLM API, Telegram) | System (dynamic link) |
| civetweb | Embedded HTTP server (webhooks, dashboard) | Yes |
| QuickJS | JS plugin engine (runtime tool creation, extensions) | Yes |

## LLM Provider

Default: OpenRouter → DeepSeek V4 Flash (`deepseek/deepseek-v4-flash`).

All providers use the OpenAI-compatible chat completions format. Switch provider via the `providers` table (or the approval-gated `request_config` action `request_changes` with a `provider` section); API keys are stored encrypted in the `secrets` table (scope `system`) via `save_secret`, referenced by name (`api_key_env`). Env var `OPENROUTER_API_KEY` is all you need to start.
