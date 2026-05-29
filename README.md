# CClaw

A minimal autonomous AI agent runtime in C. Turn-based execution, SQLite persistence, Unix process model. Adaptable via MicroQuickJS plugins.

## Design Philosophy

- **Agent runtime first** — excellent at running agent loops across providers.
- **Daemon as init** — schedules, forks, reaps. Never executes LLM logic.
- **Agents as users** — each gets a home directory, own DB, own workspace.
- **Processes are disposable** — one turn, then exit. Memory fully reclaimed.
- **Exit codes as IPC** — agents signal intent, daemon reads details from DB.
- **Config via environment** — injected at fork, immutable for process lifetime.

## Architecture

```
┌──────────┐  ┌──────────────┐  ┌──────────┐  ┌──────────────┐
│   CLI    │  │ Telegram poll│  │ Civetweb │  │  Cron thread │
│(standalone│  │   (thread)   │  │(webhooks)│  │   (thread)   │
│ process) │  └──────┬───────┘  └────┬─────┘  └──────┬───────┘
└────┬─────┘         │               │               │
     │               ▼               ▼               ▼
     │        ┌─────────────────────────────────────────┐
     │        │         inbox_insert + signal pipe      │
     │        └──────────────────┬──────────────────────┘
     │                           │
     │        ┌──────────────────▼──────────────────────┐
     │        │              DAEMON                      │
     │        │  epoll: signal pipe + SIGCHLD self-pipe  │
     │        │  fork agent on inbox signal              │
     │        │  reap children, dispatch on exit code    │
     │        └──────┬─────────────────┬────────────────┘
     │               │                 │
     │     ┌─────────▼───┐   ┌────────▼────────┐
     │     │ Agent proc  │   │  Agent proc     │
     │     │ (forked)    │   │  (forked)       │
     │     │ setrlimit   │   │  setrlimit      │
     │     │ drain inbox │   │  drain inbox    │
     │     │ LLM loop    │   │  LLM loop       │
     │     │ exit(code)  │   │  exit(code)     │
     │     └─────────────┘   └─────────────────┘
     │
     │  (CLI runs agent loop directly — no daemon needed)
     ▼
┌─────────────────────────────────────────────────────────┐
│                  3-DB SQLite (WAL)                       │
│  cclaw.db (registry/config) · agent.db · journal.db     │
└─────────────────────────────────────────────────────────┘
```

**Daemon mode** (`--daemon`): epoll loop forks isolated agent processes per session turn. Dispatches on exit code (0=deliver, 2=spawn, 3=approval, 4=config).

**CLI mode** (default): single process, runs the agent loop directly against `~/.cclaw/agents/default/agent.db`.

**Agent processes**: sandboxed with setrlimit (memory/CPU caps). Drain inbox → LLM loop → write response → exit with intent code.

## Requirements

- Linux (any arch: ARM64, ARMv7, ARMv5TE, x86_64, RISC-V)
- libcurl (system, dynamic link — sole runtime dependency)
- An OpenAI-compatible API key (default: OpenRouter)

## Quick Start

```bash
export OPENROUTER_API_KEY="sk-or-v1-..."
make
./build/cclaw
```

That's it. First run creates `~/.cclaw/` with everything needed.

## Configuration

Config resolution (highest priority first):

1. `CCLAW_*` env vars (works everywhere: Lambda, Workers, daemon fork)
2. `~/.cclaw/cclaw.db` kv table (persistent, encrypted secrets)
3. `OPENROUTER_API_KEY` env var (system-level fallback)
4. `~/.cclaw/config.json` (optional convenience file)

```
~/.cclaw/
├── cclaw.db           ← system registry + config
├── .cclaw_key         ← encryption key (mode 0600)
├── journal.db         ← all logs
├── config.json        ← optional fallback
└── agents/
    └── default/
        ├── agent.db   ← sessions, entries, memory
        └── workspace/ ← agent-created files
```

## Usage

```bash
./build/cclaw                    # interactive CLI (default agent)
./build/cclaw -p "hello"         # single-turn: print response and exit
./build/cclaw -s 3               # resume session 3
./build/cclaw --new              # force new session
./build/cclaw --daemon           # run as daemon (telegram, web, cron)
./build/cclaw --debug            # show raw LLM request/response JSON
./build/cclaw --help             # show all options
```

## Documentation

- [SPEC.md](SPEC.md) — full specification, invariants, and task list
- [REFACTOR.md](REFACTOR.md) — architecture decisions, DB split, exit code protocol
- [specs/](specs/) — detailed reference docs (schema, daemon, memory, providers, security)
- [AGENTS.md](AGENTS.md) — project ethos, coding conventions, build instructions
