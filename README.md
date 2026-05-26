# CClaw

A minimal autonomous AI agent in C. Telegram, CLI, and web dashboard. SQLite for persistence. MicroQuickJS for runtime tool creation.

## Architecture

```
┌──────────┐  ┌──────────────┐  ┌──────────┐  ┌──────────────┐
│   CLI    │  │ Telegram poll│  │ Civetweb │  │  Cron thread │
│(separate │  │   (thread)   │  │(webhooks)│  │   (thread)   │
│ process) │  └──────┬───────┘  └────┬─────┘  └──────┬───────┘
└────┬─────┘         │               │               │
     │               ▼               ▼               ▼
     │        ┌─────────────────────────────────────────┐
     │        │  inbox_insert(session_id) + signal pipe │
     │        └──────────────────┬──────────────────────┘
     │                           │
     │        ┌──────────────────▼──────────────────────┐
     │        │              DAEMON                      │
     │        │  epoll: signal pipe + SIGCHLD self-pipe  │
     │        │  fork agent on inbox signal              │
     │        │  reap children, deliver responses        │
     │        └──────┬─────────────────┬────────────────┘
     │               │                 │
     │     ┌─────────▼───┐   ┌────────▼────────┐
     │     │ Agent proc  │   │  Agent proc     │
     │     │ (forked)    │   │  (forked)       │
     │     │ landlock    │   │  landlock       │
     │     │ setrlimit   │   │  setrlimit      │
     │     │ drain inbox │   │  drain inbox    │
     │     │ LLM loop    │   │  LLM loop       │
     │     │ exit        │   │  exit           │
     │     └─────────────┘   └─────────────────┘
     │
     │  (no daemon? CLI runs agent loop directly)
     ▼
┌─────────────────────────────────────────────────────────┐
│                     SQLite (WAL)                         │
│  sessions · entries · inbox · cron · FTS5               │
└─────────────────────────────────────────────────────────┘
```

**Daemon mode** (`--daemon`): epoll loop forks isolated agent processes per session turn. The daemon never executes LLM logic — it only schedules, forks, and reaps.

**CLI mode** (default): single process, runs the agent loop directly. Detects if a daemon is running via named FIFO — if so, delegates sub-agent spawning to the daemon.

**Agent processes**: sandboxed with landlock (workspace-only writes) and setrlimit (memory/CPU caps). Drain inbox → LLM loop → write response → exit.

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

## Usage

```bash
./build/cclaw                    # interactive CLI (default)
./build/cclaw -p "hello"         # single-turn: print response and exit
./build/cclaw -s 3               # resume session 3
./build/cclaw --new              # force new session
./build/cclaw --daemon           # run as daemon (telegram, web, cron)
./build/cclaw --debug            # show raw LLM request/response JSON
./build/cclaw config.json        # explicit config file
./build/cclaw --help             # show all options
```

## Documentation

- [SPEC.md](SPEC.md) — full specification, invariants, and task list
- [specs/](specs/) — detailed reference docs (schema, daemon, memory, providers)
- [AGENTS.md](AGENTS.md) — project ethos, coding conventions, build instructions
