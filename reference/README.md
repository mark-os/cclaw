# Reference Code

Cloned repos and notes for studying patterns. Subfolders are gitignored — only this README and `*.md` notes are tracked.

## Agent Projects

```bash
# Pi — clean agent loop, session tree model (TypeScript)
git clone --depth 1 https://github.com/earendil-works/pi.git reference/pi

# OpenClaw — autonomy, security, multi-channel integration (TypeScript)
git clone --depth 1 https://github.com/openclaw/openclaw.git reference/openclaw

# NullClaw — Zig clone of OpenClaw, minimal binary
git clone --depth 1 https://github.com/nullclaw/nullclaw.git reference/nullclaw

# IronClaw — Rust agent framework, zero-trust sandboxing, WASM plugins
git clone --depth 1 https://github.com/Midoshy/ironclaw.git reference/ironclaw

# ZeroClaw — Rust, ultra-lightweight OpenClaw alternative (~8.8MB binary)
git clone --depth 1 https://github.com/zeroclaw-labs/zeroclaw.git reference/zeroclaw

# GoClaw — Go, OpenClaw rebuilt with multi-tenant isolation and native concurrency
git clone --depth 1 https://github.com/nextlevelbuilder/goclaw.git reference/goclaw
```

## C Libraries (reference only, not cloned)

```bash
# Redis — C data structures, event loop, networking patterns
# https://github.com/redis/redis

# NNG (nanomsg next gen) — lightweight brokerless messaging in C
# https://github.com/nanomsg/nng
```

## Notes

- `pi-ai.md` — LLM wire formats, caching, streaming, provider normalization
- `pi-agents.md` — Agent loop, session trees, compaction, built-in features
- `pi-extensions.md` — Plugin system, skills, tools, event hooks
