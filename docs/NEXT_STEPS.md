# CClaw — Next Steps

## What's Done

- Project structure in place (`src/`, `include/`, `test/`, `docs/`, `vendor/`, `reference/`)
- Pi and OpenClaw cloned in `reference/` for study
- `docs/REFERENCE_MAP.md` documents Pi's LLM client, agent loop, and OpenClaw's Telegram integration
- NotebookLM "CClaw" notebook has deep research on C event loops, data structures, and architecture
- Gemini research suggests: Redis `ae.c`, libcurl multi, cJSON, state machine, arena allocators

## Open Questions (Not Ready to Decide)

### Event Loop / Networking Foundation

Options under consideration:
- **Redis `ae.c`** — minimal, proven, easy to extract. Pair with libcurl for HTTP.
- **Mosquitto** — already has event loop + networking + TLS. Could build on top of it directly rather than extracting pieces. BSD-3-Clause licensed.
- **Mongoose** — embedded networking lib with built-in HTTP client/server, event loop, TLS. Single-file.
- **Custom epoll/kqueue wrapper** — maximum control, more work.

Need to: clone each, read the code, understand what you'd get for free vs what you'd fight.

### HTTP Client

- libcurl is the safe bet for outbound HTTPS (LLM APIs, Telegram Bot API)
- But if building on Mosquitto or Mongoose, they have their own HTTP client capabilities
- Question: is libcurl's multi-socket integration worth the complexity, or is blocking-per-request acceptable for a bot that handles one chat at a time?

### Streaming (SSE)

- OpenAI streams responses as `data: {...}\n\n` Server-Sent Events
- Need to parse chunks incrementally as they arrive
- This works naturally with libcurl's WRITEFUNCTION callback
- But: streaming is optional for v1. Can start with non-streaming (wait for full response)

### Concurrency

- Single-threaded event loop is the goal
- But SQLite writes and long-running tools may need a worker thread
- Telegram long-polling + LLM requests can coexist on one curl multi handle
- Multiple chats = multiple AgentContexts sharing one event loop

### Persistence

- SQLite amalgamation is lightweight and proven
- WAL mode for non-blocking reads
- Worker thread for writes, or just accept brief blocks?
- JSONL transcript files (like OpenClaw) vs SQLite tables?

### Build System

- CMake vs plain Makefile vs Meson
- Cross-compilation needs (armv5 mentioned in research)

## Next Actions

1. **Clone candidate C projects into `vendor/`** — Redis (for ae.c), Mosquitto, Mongoose, cJSON, sqlite3 amalgamation. Read their event loops, compare.

2. **Prototype: minimal Telegram bot in C** — Pick one networking approach, get `getUpdates` long-polling working, echo messages back. This will force a decision on the networking stack.

3. **Prototype: minimal OpenAI chat completion call** — Send a message, get a response, parse it with cJSON. Non-streaming first.

4. **Decide on foundation** — After prototyping, pick the event loop + HTTP approach.

5. **Design the agent loop** — State machine, message history, tool registry. The struct layout from the research report is a solid starting point.

6. **Wire it together** — Telegram → agent loop → OpenAI → reply.

## References

- `docs/REFERENCE_MAP.md` — Pi/OpenClaw architecture + NotebookLM research
- `reference/pi/` — Pi monorepo (agent loop, LLM client)
- `reference/openclaw/` — OpenClaw (Telegram extension, Pi embedding)
- NotebookLM "CClaw" notebook — 6 sources on C architecture
