# Letta Notes

State of the art as of July 2026. Formerly MemGPT, rewritten from Python server to TypeScript agent runtime.

## Architecture

Letta Code is a local-first stateful agent harness. The agent runs as a process (interactive TUI or headless) and maintains persistent memory across sessions.

```
┌─────────────────────────────────────────────────────┐
│  Surfaces                                           │
│  CLI (Ink/React TUI) · Desktop App · chat.letta.com │
│  Slack · Discord · Telegram · WhatsApp · Signal     │
└────────────────────────┬────────────────────────────┘
                         │ WebSocket / stdin
┌────────────────────────▼────────────────────────────┐
│  letta-code runtime                                 │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐            │
│  │ Agent    │ │ Memory   │ │ Tools    │            │
│  │ Loop     │ │ (MemFS)  │ │ & Skills │            │
│  └──────────┘ └──────────┘ └──────────┘            │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐            │
│  │ Mods     │ │ Channels │ │ Subagents│            │
│  └──────────┘ └──────────┘ └──────────┘            │
└────────────────────────┬────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────┐
│  LLM Providers (via @earendil-works/pi-ai)          │
│  Anthropic · OpenAI · zAI · OpenRouter · Ollama     │
│  LM Studio · llama.cpp · Bedrock · MiniMax          │
└─────────────────────────────────────────────────────┘
```

## Memory System

Memory is a git-backed filesystem at `~/.letta/agents/<id>/memory/`.

- **Memory blocks** — `.mdx` files (primarily `persona` and `human`) that the agent reads and rewrites. These are injected into the system prompt.
- **Git versioning** — every memory write is a commit. Full history, diffs, branches via worktrees.
- **Remote sync** — push memory to a GitHub repo via `/memory-repository set git@github.com:...`
- **Git signing** — integrity guarantees on memory commits.
- **Dreaming** (`letta dream` / `/sleeptime`) — offline reflection passes. A subagent reviews conversation history and consolidates learnings into memory blocks. Runs on a schedule or on-demand.
- **Memory tools** — the agent can read, write, and patch its own memory programmatically.

The agent's long-term learning loop: interact → reflect (dream) → rewrite memory → next interaction starts with updated context.

## Modes

- **Interactive** — Ink TUI with statusline, approval diffs, panels, input. This is the `letta` command.
- **Headless** — no TUI. Used by channels, crons, the SDK, and `letta server`. Full agent loop without terminal interaction.
- **Server** (`letta server`) — headless mode with a WebSocket app-server. Desktop app and chat.letta.com connect to this.

## Surfaces

| Surface | Connection |
|---------|-----------|
| CLI (TUI) | Direct, in-process |
| Desktop app (closed source) | Local WebSocket to app-server |
| chat.letta.com | Via Constellation cloud relay |
| Mobile browser | Via Constellation (chat.letta.com) |
| Slack, Discord, Telegram, WhatsApp, Signal | Channel adapters in headless mode |
| Custom channels | Plugin API |
| SDK (`@letta-ai/letta-agent-sdk`) | Programmatic, can run local or via Constellation |

All surfaces talk to the same agent with the same memory. Switch between CLI and Slack mid-conversation.

## Mods (Runtime Extensions)

npm packages that extend the agent runtime. Installed via `letta install npm:@letta-ai/<mod>`.

**What mods can add:**
- Tools (agent-callable functions)
- Slash commands (user-facing)
- Lifecycle/turn/tool-call hooks
- Permission checks
- Provider adapters
- UI surfaces (panels, statusline segments)

**Notable mods (from `letta-ai/mods`):**

| Mod | Purpose |
|-----|---------|
| plan-mode | Plan-mode workflow |
| goal-mode | Goal tracking with turn reminders |
| autopivot | Model failover on rate limits/errors |
| hypa | Compress noisy tool output before context |
| output-compressor | Reversible large-output compression |
| muscle-memory | Auto-extract skills from tool-use patterns |
| oath-keeper | Detect promises, auto-deliver |
| threadkeeper | Track commitments and open loops |
| soft-landing | Recovery from drift/compaction/overload |
| memfs-search | Agent-callable memory search |
| web-search | Provider-backed web search |
| image-understanding | Vision bridge for text-only models |
| control-room | Cockpit for goal/progress/approval state |
| environment-compass | Read-only env/git orientation |

Mods are distinct from **skills** (declarative knowledge/procedures). Mods are executable runtime code; skills are documents the agent reads.

## Provider Layer

Uses `@earendil-works/pi-ai` (the Pi project's provider library) for LLM normalization. Supports:
- Anthropic, OpenAI, zAI, OpenRouter, Ollama, LM Studio, llama.cpp, Bedrock, MiniMax
- Provider mods can add custom adapters
- Model switching via `/model`
- API keys configured via `/connect`

## Subagents

Built-in subagent types: general-purpose, forked, recall, history-analyzer. Agents can call any other agent (including themselves) as subagents, async or sync.

Dreaming uses a dedicated reflection subagent.

## Deployment

**Local:** `npm install -g @letta-ai/letta-code && letta`

**Always-on remote:** Deploy `letta server` via Docker/Fly/Railway/$4 droplet. Opens outbound WebSocket to Constellation — no inbound ports needed. Desktop app and chat.letta.com connect through the relay.

**Constellation** (Letta's cloud layer): handles remote env routing, secrets sync, OAuth. Optional — agent works fully offline without it.

## Repos

| Repo | Role |
|------|------|
| `letta-ai/letta-code` | Agent runtime — the main thing |
| `letta-ai/mods` | 23 first-party mod packages |
| `letta-ai/letta-agent-sdk` | TypeScript SDK for building on Letta Code |
| `letta-ai/skills` | Shared skills (cross-agent, works with Claude Code too) |
| `letta-ai/letta-app-server-deployment` | Docker/Fly/Railway deployment template |
| `letta-ai/social-cli` | Connect AI to social web |
| `letta-ai/letta-oss-ui` | Open-source demo desktop UI (fork of Claude-Cowork) |
| `letta-ai/letta` | Legacy Python server (maintenance only) |

## Evolution (brief)

The old letta was a Python FastAPI server with Postgres, SQLAlchemy ORM, Alembic migrations, and memory stored as database rows. Multi-tenant managed deployment.

They dropped all of it. The new system is:
- Local process, not a cloud service
- Files on disk, not database rows
- Git versioning, not migration scripts
- npm packages, not Python plugins
- React TUI, not API-only

The core insight (agents with structured self-modifying memory) survived. The substrate changed completely. Memory went from opaque DB rows to markdown files you can `git log`, `grep`, and push to GitHub.
