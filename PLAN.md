# CClaw — Plan

## What This Is

A minimal AI agent in C. Telegram bot + CLI → agent loop → OpenAI-compatible LLM → tool execution. Targets ARMv5TE (Pogoplug V4) but develops on ARM64 (EC2 t4g.small).

## Architecture Decisions (Settled)

- **Language**: C11, compiled with GCC
- **Concurrency**: Thread-per-conversation, blocking I/O within each thread. No event loop.
- **HTTP**: libcurl (blocking `curl_easy_perform`), one handle per thread
- **JSON**: cJSON (vendored, single file)
- **Persistence**: SQLite amalgamation (vendored)
- **Atomics on ARMv5**: GCC `__atomic` builtins + `-latomic` (no custom shim needed)
- **Build**: Makefile. Cross-compile for ARMv5 with musl toolchain later.
- **Streaming**: Not for v1. Full response, then parse.
- **First channel**: CLI (stdin/stdout). Telegram second.
- **First tool**: `shell_exec` — proves the full agent loop end-to-end.

## Development Approach

- **TDD**: Write tests first, implement until they pass.
- **CLI-first**: Interactive terminal chat is the first milestone. Telegram comes after.
- **Dev on ARM64 EC2** (t4g.small), deploy to ARMv5 Pogoplug when ready.
- **Reference projects**: Pi (agent loop patterns), OpenClaw (Telegram integration), nullclaw (Zig implementation, deployment scripts).
- **Small steps**: Each bead is a meaningful, testable unit. `br ready` shows what's next.

## Phases

### Phase 1 — Working CLI agent with shell tool (P1 beads)
Vendor deps → Makefile → arena allocator → core types → HTTP client → LLM client (build/parse/call) → CLI channel → agent loop (text only) → agent loop (tool calls) → shell_exec tool → wired main()

**Milestone**: `./cclaw` starts, you type a message, it calls an LLM, the LLM can run shell commands, you see the result.

### Phase 2 — Persistence + Telegram + more tools (P2 beads)
Config file → SQLite persistence → file_read/file_write tools → Telegram polling + send → multi-channel dispatcher with worker threads

**Milestone**: Bot runs as a daemon, responds on Telegram, remembers conversation history across restarts.

### Phase 3 — Pogoplug deployment (P3 beads)
Cross-compile with musl for ARMv5TE → deploy script → service management on device

**Milestone**: Single static binary runs on Pogoplug, accessible via Telegram.

## Future (Not Planned Yet)

- Workspaces and path sandboxing (Linux Landlock)
- Embedded scripting language for extensibility
- SSE streaming for faster perceived response
- Multiple LLM provider support / fallback
- Memory / RAG

## Dependencies

| Dep | Source | License | Vendored? |
|-----|--------|---------|-----------|
| cJSON | github.com/DaveGamble/cJSON | MIT | Yes |
| SQLite | sqlite.org | Public domain | Yes |
| libcurl | system / static build | MIT-like | Linked |

## Dev Environment

- **Machine**: EC2 t4g.small (ARM64, Amazon Linux 2023)
- **Tools**: gcc, make, git, libcurl-devel, valgrind, br (beads_rust)
- **Repos on machine**: cclaw (working), reference/pi, reference/openclaw, reference/nullclaw
