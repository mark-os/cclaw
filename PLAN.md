# CClaw — Plan

## What This Is

A minimal AI agent in C. Telegram bot + CLI → agent loop → OpenAI-compatible LLM → tool execution. Targets ARMv5TE (Pogoplug V4) but develops on ARM64 (EC2 t4g.small).

## Architecture Decisions (Settled)

- **Language**: C11, compiled with GCC
- **Concurrency**: Thread-per-conversation, blocking I/O within each thread. No event loop.
- **HTTP**: libcurl (blocking `curl_easy_perform`), one handle per thread
- **JSON**: cJSON (vendored, single file)
- **Persistence**: SQLite amalgamation (vendored), tree-ready schema (entries with id/parent_id)
- **Session model**: Unidirectional in Phase 1 (append, delete-from-end, summarize-and-trim). Tree-ready schema supports future branching without migration. See docs/SESSION_TREE.md.
- **Atomics on ARMv5**: GCC `__atomic` builtins + `-latomic` (no custom shim needed)
- **Build**: Makefile. Cross-compile for ARMv5 with musl toolchain later.
- **Streaming**: Not for v1. Full response, then parse.
- **First channel**: CLI (stdin/stdout). Telegram second.
- **First tool**: `shell_exec` — proves the full agent loop end-to-end.
- **Scripting**: MicroQuickJS (Bellard, 10KB RAM, ARM Thumb-2). See docs/SCRIPTING.md.
- **Test provider**: OpenRouter (OpenAI-compatible). Primary model: `deepseek/deepseek-v4-flash`. Alt: NVIDIA NIM. See docs/MODELS.md.
- **Config (Phase 1)**: Env vars only (`OPENROUTER_API_KEY`, optional `CCLAW_BASE_URL`, `CCLAW_MODEL`). Config file in Phase 2.
- **Shell restriction**: Hardcoded allowlist in Phase 1 (testing safety). Will be replaced with workspace-based sandboxing later.

## Development Approach

- **TDD**: Write tests first, implement until they pass.
- **CLI-first**: Interactive terminal chat is the first milestone. Telegram comes after.
- **Dev on ARM64 EC2** (t4g.small), deploy to ARMv5 Pogoplug when ready.
- **Reference projects**: Pi (agent loop, session tree, branching), OpenClaw (Telegram integration, sub-agents, cron), nullclaw (Zig implementation, deployment scripts).
- **Small steps**: Each bead is a meaningful, testable unit. `br ready` shows what's next.

## Phases

### Phase 1 — Working CLI agent with shell tool (P1 beads)
Vendor deps → Makefile → arena allocator → core types → HTTP client → LLM client (build/parse/call) → CLI channel → agent loop (text only) → agent loop (tool calls) → shell_exec tool → wired main()

Config via env vars. In-memory message history (flat array, but using entry structs compatible with tree schema).

**Milestone**: `./cclaw` starts, you type a message, it calls an LLM, the LLM can run shell commands, you see the result.

### Phase 2 — Persistence + Telegram + sub-agents (P2 beads)
Config file → SQLite persistence (tree-ready schema) → file_read/file_write tools → Telegram polling + send → multi-channel dispatcher with worker threads → minimal sub-agent spawn

See docs/PHASE2.md for full spec.

**Milestone**: Bot runs as a daemon, responds on Telegram, remembers conversation history across restarts, can spawn background sub-agents.

### Phase 3 — Autonomy + Pogoplug deployment (P3 beads)
Heartbeats → cron/scheduled tasks → full sub-agents (depth, roles, cancellation) → session branching → compaction → MicroQuickJS scripting → cross-compile with musl for ARMv5TE → deploy script → service management

See docs/PHASE3.md for full spec.

**Milestone**: Proactive agent that wakes itself, manages tasks, branches conversations, runs JS tools, deployed as a static binary on Pogoplug.

## Dependencies

| Dep | Source | License | Vendored? |
|-----|--------|---------|-----------|
| cJSON | github.com/DaveGamble/cJSON | MIT | Yes |
| SQLite | sqlite.org | Public domain | Yes |
| libcurl | system / static build | MIT-like | Linked |
| MicroQuickJS | github.com/bellard/mquickjs | MIT | Yes (Phase 3) |

## Dev Environment

- **Machine**: EC2 t4g.small (ARM64, Amazon Linux 2023)
- **Tools**: gcc, make, git, libcurl-devel, valgrind, br (beads_rust)
- **Repos on machine**: cclaw (working), reference/pi, reference/openclaw, reference/nullclaw
- **LLM API**: OpenRouter (`$OPENROUTER_API_KEY`), model: `deepseek/deepseek-v4-flash`. Alt: NVIDIA NIM (`$NVIDIA_API_KEY`)

## Design Docs

- `docs/SESSION_TREE.md` — Session tree schema and operations
- `docs/PHASE2.md` — Phase 2 detailed spec
- `docs/PHASE3.md` — Phase 3 detailed spec
- `docs/SCRIPTING.md` — MicroQuickJS integration plan
- `docs/MODELS.md` — Model selection and provider config
- `docs/REFERENCE_MAP.md` — Detailed map of Pi and OpenClaw internals
