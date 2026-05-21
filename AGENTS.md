# CClaw — Agent Instructions

## Project Ethos

CClaw is a **minimal** AI agent in C. The goal is the smallest correct implementation that works on constrained hardware (ARMv5TE Pogoplug with 128MB RAM). Every line of code must earn its place.

**Inspiration**

CClaw draws inspiration and shamelessly steals ideas from *Pi agent*, cloned in `reference/pi-mono`, and the autonomous agent *OpenClaw*, in `reference/openclaw`, that was based on pi agent. Pi was intended from the start to be extremely minimal, functional, and easily self-modifying. CClaw attempts to be as adaptable to different tasks as OpenClaw, through a plugin system based on MicroQuickJS by Fabrice Bellard. With that said, CClaw does not try to be everything to everyone. It does not have all the connectors to different communication channels for instance, as it is intended to serve the needs of its creator Mark Ostroth. Finally it draws lessons from *nullclaw*, cloned in ~/nullclaw, a standalone Zig program that is also a clone of OpenClaw. However, CClaw does not intend to have a "pure Zig, cross compilation everywhere" attitude and will borrow C libraries or even link dynamically to system curl, whatever it takes to produce a simple, usable, excellent autonomous agent.

**Principles:**
- Simple over clever. Blocking I/O over event loops. Threads over callbacks.
- Reference Pi for agent loop patterns (clean, battle-tested TypeScript).
- Reference OpenClaw for integration patterns, security measures, usability features, and permission/authorization patterns.
- One tool at a time. `shell_exec` first, prove the loop works, then add more.
- Session/messages on heap (individually owned). Arena for per-turn scratch only.

## Memory Model

See `docs/ARCHITECTURE.md` for full details. Summary:

- **Session** (`session_create/destroy`): heap-owned growable message array. Each message owns its strings via `malloc`. Lives for conversation duration.
- **Per-turn Arena** (`arena_create/destroy`): 512KB scratch for LLM request/response JSON, tool output, telegram parsing. Created fresh each turn, destroyed after.
- **Config Arena**: small (4KB), lives for process lifetime. Read-only after load.
- **AgentContext**: per-turn struct referencing `{session, arena, config}`. Passed to agent/llm functions.

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
vendor/        Vendored libs (cJSON, sqlite3)
test/          Test files (test_*.c)
reference/     Pi, OpenClaw, nullclaw clones (gitignored)
build/         Build output (gitignored)
docs/          Design docs
```

## Building

```bash
make              # native build (x86_64)
make test         # run all 18 tests
make clean        # remove build/
```

### Cross-compile for Pogoplug (ARMv5TE)

Requires `gcc-arm-linux-gnueabi` and curl headers from the device:

```bash
# One-time setup: copy curl headers from device
ssh pogoplug 'apt-get install -y libcurl4-openssl-dev'
scp -r pogoplug:/usr/include/arm-linux-gnueabi/curl vendor/curl

# Copy libcurl.so for linking
scp pogoplug:/usr/lib/arm-linux-gnueabi/libcurl.so.4.8.0 vendor/
ln -sf libcurl.so.4.8.0 vendor/libcurl.so

# Build
arm-linux-gnueabi-gcc -std=c11 -Wall -Wextra -Werror -O2 \
    -march=armv5te -marm \
    -Iinclude -Ivendor/cJSON -Ivendor/sqlite3 -Ivendor \
    src/*.c vendor/cJSON/cJSON.c vendor/sqlite3/sqlite3.c \
    -Lvendor -lcurl -lpthread -ldl -lm \
    -Wl,--allow-shlib-undefined \
    -o build/cclaw-arm

# Deploy
scp build/cclaw-arm pogoplug:/usr/local/bin/cclaw
```

The device runs Debian Bookworm (armel) with libcurl and all its deps installed via apt. We link dynamically against curl (duct tape: `--allow-shlib-undefined` skips transitive deps at link time, resolved at runtime on device).

