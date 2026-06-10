# CClaw

A minimal autonomous AI agent runtime in C. Single binary, multi-agent, fork+exec for isolation. Runs anywhere libcurl does.

## Quick Start

```bash
export OPENROUTER_API_KEY="sk-or-v1-..."
make
./build/cclaw
```

First run creates `~/.cclaw/`, bootstraps a default agent, and walks you through setup interactively.

## Architecture

CClaw combines threads for concurrency with fork+exec for isolation. The main process uses a poll loop (not an event loop — no callbacks, no async) with worker threads for LLM calls. Tool execution forks+execs to sandbox untrusted work in separate address spaces.

```
Main process
├─ main thread: poll loop (stdin, signals, worker notifications)
├─ worker threads (1–N elastic, each owns sqlite3* + CURL*)
│    LLM HTTP calls, connection reuse, 30s idle timeout
│
├─ fork+exec → shell children (sandboxed: namespaces, no network)
└─ fork+exec → channel runners (long-lived, per messaging platform)
```

All state lives in a single SQLite WAL database (`cclaw.db`). Sessions, entries, config, memory, secrets, job queue — one file, one source of truth. WAL mode allows concurrent reads and writes across threads without blocking.

**CLI mode** (default): main process with worker thread pool. Simple, fast, no daemon.

**Daemon mode** (`--daemon`): adds Telegram polling, webhook server, cron scheduling, and multi-agent coordination. Worker threads handle concurrent sessions. `--llm-fork` is available as a fallback that fork+execs each LLM call for full process isolation at the cost of connection reuse.

## Secure

Agent tool calls are sandboxed at multiple levels:

- **Shell children** run in Linux namespaces (CLONE_NEWUSER, CLONE_NEWNET, CLONE_NEWNS). Filesystem restricted to workspace (rw) + system dirs (ro). No direct network access.
- **Network proxy** — shell children that need HTTP connect back to the parent via a Unix domain socket. The parent enforces a per-agent host allowlist before forwarding. No unvetted outbound connections.
- **Workspace isolation** — file tools are scoped to `agents/<name>/workspace/`. No traversal, no access to other agents' data.
- **Secrets** — encrypted at rest in cclaw.db (ChaCha20-Poly1305, via Monocypher). Decrypted only at runtime, injected via env, never exposed to the model or logged.
- **Resource limits** — agent processes get `setrlimit` caps (memory, CPU time) preventing runaway consumption.

## Portable

CClaw vendors everything except libcurl:

| Vendored | Purpose |
|----------|---------|
| SQLite 3.53 | All persistence |
| civetweb | Embedded HTTP server |
| MicroQuickJS | JS plugin engine |
| Monocypher | Encryption |

**Runtime dependency:** `libcurl.so` (system-provided, dynamically linked). Available on every Linux distribution, every architecture.

**Build dependency:** a C11 compiler. That's it.

**Targets:** ARM64, ARMv7, ARMv5TE, x86_64, RISC-V. Tested on EC2 t4g.small, Chromebooks, and a Pogoplug V4 (128MB RAM, ARMv5TE).

```
Binary size: 2.1 MB (1.5 MB is SQLite)
Peak RSS:    9.7 MB (single turn)
Startup:     10 ms
```

## Self-Configuring

Agents can request their own reconfiguration through exit codes and tool calls:

- **Provider configuration** — agents call `configure_provider` to set API keys, models, endpoints
- **Channel configuration** — agents call `configure_channel` to set up Telegram bots or webhooks
- **Agent creation** — agents call `create_agent` to spawn new agents with custom system prompts and tool permissions
- **Approval flow** — agents can request human approval for sensitive operations (exit code 3 → daemon queues for approval)
- **Escalation** — a sub-agent that hits its limits can escalate to its parent agent

Configuration is hierarchical: env vars override DB values override defaults. Agents read from DB but can only write through sanctioned tool calls.

## Extensible

The MicroQuickJS plugin system lets agents load JavaScript extensions at runtime:

- **Channel plugins** — JS files that implement polling/sending for messaging platforms (Telegram ships built-in, others addable)
- **Runtime tools** — agents can define new tools via `js_define_tool`, expanding their own capabilities mid-session
- **Custom logic** — extensions loaded from `agents/<name>/workspace/` at startup, scoped per-agent

Channels are self-contained JS programs that speak a simple protocol: poll for messages, emit events to the DB, read outbox for responses. The daemon manages their lifecycle (spawn, monitor, restart).

## Self-Bootstrapping

On first run with just an API key, CClaw:

1. Creates a bootstrap agent with elevated permissions
2. The bootstrap agent walks the user through setup conversationally
3. User provides secrets (API keys, bot tokens) via `request_config` — the agent asks, the CLI collects input, encrypts it, stores it. **The secret value is never shown to the model.**
4. The bootstrap agent creates a permanent agent with appropriate tools and permissions
5. Setup complete — the bootstrap agent demotes itself

No manual config files. No YAML. The agent configures itself through the same tool-call interface it uses for everything else.

## Usage

```bash
cclaw                    # interactive CLI (default agent, streaming)
cclaw -p "hello"         # single-turn: print response and exit
cclaw -s 3               # resume session 3
cclaw --new              # force new session
cclaw --daemon           # daemon mode (channels, cron, multi-agent)
cclaw -v                 # debug logging (timing, SQL profiling)
cclaw -vv                # trace logging (full LLM req/resp JSON)
cclaw --llm-fork         # use fork-per-call instead of thread pool
cclaw --help             # all options
```

## Configuration

Priority (highest first):

1. `CCLAW_*` env vars
2. `~/.cclaw/cclaw.db` kv table
3. `OPENROUTER_API_KEY` env var (convenience fallback)

```
~/.cclaw/
├── cclaw.db           ← all state
├── .cclaw_key         ← encryption key (mode 0600)
└── agents/
    └── default/
        └── workspace/ ← agent file sandbox
```

## Building

```bash
make              # build ./build/cclaw
make test         # unit tests (no network)
make debug        # ASAN + UBSan build (clang, -O0 -g3)
make clean        # remove build/
```

## Benchmarks

Measured on Acer Chromebook Plus 514 (Intel i3-N305, 8GB RAM, Linux container).

```
Agent overhead:  ~13 ms (DB + context build, before network)
TTFB breakdown (OpenRouter, DeepSeek V4 Flash):
  CClaw:         13 ms
  DNS+TCP+TLS:  113 ms
  LLM:         1073 ms (model-dependent)
```

## Documentation

- [SPEC.md](SPEC.md) — specification, invariants, task list
- [specs/](specs/) — reference docs (schema, daemon, memory, providers, security)
- [AGENTS.md](AGENTS.md) — project ethos, coding conventions, build instructions

## Acknowledgements

### Vendored Libraries

| Library | License |
|---------|---------|
| [SQLite 3.53.1](https://sqlite.org) | Public Domain |
| [civetweb 1.16](https://github.com/civetweb/civetweb) | MIT |
| [MicroQuickJS](https://github.com/nicholasgasior/mquickjs) | MIT |
| [Monocypher 4.0.2](https://monocypher.org) | BSD-2-Clause / CC0-1.0 |

### Inspiration

| Project | What we learned |
|---------|-----------------|
| Pi agent | Clean agent loop, session tree model |
| OpenClaw | Autonomy features, security patterns, multi-channel integration |
| nullclaw | Architecture reference (Zig clone of OpenClaw) |
| Letta | Stateful agent design, persistent memory patterns |
| IronClaw | Secure execution model, secret injection |

Special thanks to Mario Zechner and Peter Steinberger for starting the Claw movement, Igor Somov for showing what's possible in less than 1MB with NullClaw, and Rhett Creighton for insisting that I vibe code in C.
