# Architecture

## Data Flow

```
Channel (CLI/Telegram) → Dispatch Queue → Worker Thread
  → Session (load from DB) → Agent Turn → LLM → Tool Loop → Response
  → Persist to DB → Send reply → Destroy turn arena
```

## Memory

Two strategies, split by lifetime:

**Heap** — Session & messages. Growable, mutable, individually freeable.
```
Session (malloc)
└── Message[] (realloc'd)
    └── each message owns its strings via malloc
```

**Arena** — Per-turn scratch. Created before turn, destroyed after.
```
Turn Arena (512KB)
├── LLM request JSON
├── LLM response buffer (128KB)
├── Parsed response fields
└── Tool output scratch
```

Config gets its own small arena (4KB, process lifetime, read-only after load).

## Modules

| Module | Responsibility |
|--------|---------------|
| `types.c` | Session/message lifecycle (heap) |
| `agent.c` | Turn loop: LLM call → tool dispatch → repeat |
| `llm.c` | Build request JSON, parse response, HTTP call |
| `config.c` | Load JSON + env vars into Config struct |
| `db.c` | SQLite persistence (messages table per chat_id) |
| `dispatch.c` | Thread-safe bounded queue (mutex + condvar) |
| `telegram.c` | Polling + send + typing indicator |
| `main.c` | CLI mode or Telegram daemon mode |

## Concurrency

- CLI: single-threaded, one session, per-turn arena
- Daemon: poller thread → dispatch queue → N worker threads
- Each worker gets its own session + arena per message (no sharing)

## Platform

Primary target: Pogoplug V4 (Marvell Kirkwood, ARMv5TE, 128MB RAM, Debian Bookworm armel).

Binary is dynamically linked against device's libcurl (apt-managed). Everything else (cJSON, SQLite) is vendored and compiled in. The binary is ~1.1MB stripped.

Designed to also run on x86_64 Linux (development) and potentially any POSIX system with libcurl available.
