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

# Hermes — NousResearch agent framework
git clone --depth 1 https://github.com/NousResearch/hermes-agent.git reference/hermes

# Letta — legacy server (active development moved to letta-code)
git clone --depth 1 https://github.com/letta-ai/letta.git reference/letta

# Letta Code — stateful agent harness, memory/identity/learning, multi-channel
git clone --depth 1 https://github.com/letta-ai/letta-code.git reference/letta-code

# Letta Mods — runtime extensions: tools, commands, hooks, providers, UI surfaces
git clone --depth 1 https://github.com/letta-ai/mods.git reference/letta-mods

# IronClaw — Rust agent framework, zero-trust sandboxing, WASM plugins
git clone --depth 1 https://github.com/nearai/ironclaw.git reference/ironclaw

# ZeroClaw — Rust, ultra-lightweight OpenClaw alternative (~8.8MB binary)
git clone --depth 1 https://github.com/zeroclaw-labs/zeroclaw.git reference/zeroclaw

# GoClaw — Go, OpenClaw rebuilt with multi-tenant isolation and native concurrency
git clone --depth 1 https://github.com/nextlevelbuilder/goclaw.git reference/goclaw
```

## Notes

- `pi-ai.md` — LLM wire formats, caching, streaming, provider normalization
- `pi-agents.md` — Agent loop, session trees, compaction, built-in features
- `pi-extensions.md` — Plugin system, skills, tools, event hooks
