# CClaw

A minimal autonomous AI agent runtime in C. Single binary, multi-agent, sandbox isolation via fork+exec for tools. Runs anywhere libcurl does.

## Quick Start

```bash
export OPENROUTER_API_KEY="sk-or-v1-..."
make
./build/cclaw
```

First run creates `~/.cclaw/`, bootstraps a default agent, and walks you through setup interactively.

## Architecture

CClaw combines threads for concurrency with fork+exec for isolation. The main process uses a poll loop (not an event loop — no callbacks, no async) with worker threads for LLM calls. Tool execution forks+execs to sandbox untrusted work in separate address spaces.

```
Main process
├─ main thread: poll loop (stdin, signals, worker notifications)
├─ worker threads (1–N elastic, each owns sqlite3* + CURL*)
│    LLM HTTP calls, connection reuse, 30s idle timeout
│
├─ fork+exec → shell children (sandboxed: namespaces, no network)
└─ fork+exec → channel runners (long-lived, per messaging platform)
```

All state lives in a single SQLite WAL database (`cclaw.db`). Sessions, entries, config, memory, secrets, job queue — one file, one source of truth. WAL mode allows concurrent reads and writes across threads without blocking.

**CLI mode** (default): main process with worker thread pool. Simple, fast, no daemon.

**Daemon mode** (`--daemon`): adds Telegram polling, webhook server, cron scheduling, and multi-agent coordination. Worker threads handle concurrent sessions.

## Secure

Agent tool calls are sandboxed at multiple levels:

- **Shell children** run in Linux namespaces (CLONE_NEWUSER, CLONE_NEWNET, CLONE_NEWNS). Filesystem restricted to workspace (rw) + system dirs (ro). No direct network access.
- **Network proxy** — shell children that need HTTP connect back to the parent via a Unix domain socket. The parent enforces a per-agent host allowlist before forwarding. No unvetted outbound connections.
- **Workspace isolation** — file tools are scoped to `agents/<name>/workspace/`. No traversal, no access to other agents' data.
- **Secrets** — encrypted at rest in cclaw.db (ChaCha20-Poly1305, via Monocypher). Decrypted only at runtime, injected via env, never exposed to the model or logged.
- **Resource limits** — `setrlimit` caps (memory, CPU time) prevent runaway consumption.

## Portable

CClaw vendors everything except libcurl:

| Vendored | Purpose |
|----------|---------|
| SQLite 3.53 | All persistence |
| civetweb | Embedded HTTP server |
| QuickJS | JS plugin engine |
| Monocypher | Encryption |

**Runtime dependency:** `libcurl.so` (system-provided, dynamically linked). Available on every Linux distribution, every architecture.

**Build dependency:** a C11 compiler. That's it.

**Targets:** ARM64, ARMv7, ARMv5TE, x86_64, RISC-V. Tested on EC2 t4g.small, Chromebooks, and a Pogoplug V4 (128MB RAM, ARMv5TE).

```
Binary size: 2.1 MB (1.5 MB is SQLite)
Peak RSS:    9.7 MB (single turn)
Startup:     10 ms
```

## Self-Configuring

Agents can request their own reconfiguration through exit codes and tool calls:

- **Provider configuration** — agents call `configure_provider` to set API keys, models, endpoints
- **Channel configuration** — agents call `configure_channel` to set up Telegram bots or webhooks
- **Agent creation** — agents call `create_agent` to spawn new agents with custom system prompts and tool permissions
- **Approval flow** — agents can request human approval for sensitive operations via tool calls
- **Escalation** — a sub-agent that hits its limits can escalate to its parent agent

Configuration is hierarchical: env vars override DB values override defaults. Agents read from DB but can only write through sanctioned tool calls.

## Built-in Tools

Every agent starts with a core toolset (more can be added at runtime — see [Extensible](#extensible)):

| Tool | Purpose |
|------|---------|
| `file_read` / `file_write` | Read a file / create or overwrite it entirely |
| `file_edit` | Targeted search/replace edits — batched, matched against the original, non-overlapping |
| `file_list` | List a directory (sorted alphabetically, `/` marks directories) |
| `file_find` | Find files by glob (`*.c`, `src/**/*.spec.ts`) |
| `file_grep` | Search file contents by POSIX regex, returns `path:line:match` |
| `shell_exec` | Run a shell command in the namespace sandbox |
| `js_eval` | Run JavaScript in the sandboxed ES5 engine |
| `web_fetch` | Fetch a URL (host-allowlisted) |
| `db_query` | Query the agent's own SQLite database |
| `memory_create` / `memory_append` / `memory_replace` | Edit persistent memory blocks |

File tools are workspace-scoped (no traversal); `file_find`/`file_grep` skip `.git` and `node_modules`. `js_eval` runs an **ES5** dialect (`var`, not `const`/`let`; no `require`/`import`) with filesystem globals `fs.readDir/readFile/writeFile/stat/cwd` plus `http_fetch` — see [specs/extensions.md](specs/extensions.md#js-runtime-dialect--globals).

Daemon mode adds orchestration tools (`create_agent`, `launch_agent`, `configure_provider`, `configure_channel`, `cron_*`); any agent can request more via `request_config`.

## Extensible

Agents extend themselves at runtime. An **extension** is the unit of sharing: a directory with an `extension.json` manifest declaring any mix of tools, hooks, a channel, scripts, [skills](specs/skills.md), and config keys, plus the QuickJS handler files it points to. The lifecycle:

1. **Draft** — the agent writes the bundle in its private workspace (`workspace/extensions/<name>/`) and tests handlers with `js_eval`. Personal skills need no bundle at all — drop a `SKILL.md` in `agents/<name>/skills/`.
2. **Promote** (`extension_promote`) — validates the manifest, copies the bundle into the agent-immutable shared store (`~/.cclaw/extensions/<name>/`), and registers its contents. Still visible only to the owning agent.
3. **Publish** (`extension_publish`) / **attach** (`extension_attach`) — other agents opt in. Attach never grants authority: tools remain subject to each agent's own grants.

The manifest is the only door from agent-level drafts to system-level registration — one choke point where everything is validated and enumerable before it registers. Channels are self-contained JS programs run as daemon-managed processes (Telegram ships built-in); extension config lands in the self-describing config registry as `<ext>.<key>`. See [specs/extensions.md](specs/extensions.md) for the authoring walkthrough.

## Self-Bootstrapping

On first run with just an API key, CClaw:

1. Creates a bootstrap agent with elevated permissions
2. The bootstrap agent walks the user through setup conversationally
3. User provides secrets (API keys, bot tokens) via `request_config` — the agent asks, the CLI collects input, encrypts it, stores it. **The secret value is never shown to the model.**
4. The bootstrap agent creates a permanent agent with appropriate tools and permissions
5. Setup complete — the bootstrap agent demotes itself

No manual config files. No YAML. The agent configures itself through the same tool-call interface it uses for everything else.

## Usage

```bash
cclaw                    # interactive CLI (default agent, streaming)
cclaw -p "hello"         # single-turn: print response and exit
cclaw -s 3               # resume session 3
cclaw --new              # force new session
cclaw --daemon           # daemon mode (channels, cron, multi-agent)
cclaw -v                 # debug logging (timing, SQL profiling)
cclaw -vv                # trace logging (full LLM req/resp JSON)
cclaw --help             # all options
```

## Configuration

Priority (highest first):

1. `CCLAW_*` env vars
2. `~/.cclaw/cclaw.db` `config` table (registry-backed: every key ships a default + description; DB rows are overrides)
3. `OPENROUTER_API_KEY` env var (convenience fallback)

```
~/.cclaw/
├── cclaw.db           ← all state
├── .cclaw_key         ← encryption key (mode 0600)
└── agents/
    └── default/
        └── workspace/ ← agent file sandbox
```

## Building

```bash
make              # build ./build/cclaw
make test         # unit tests (no network)
make debug        # ASAN + UBSan build (clang, -O0 -g3)
make clean        # remove build/
```

## Benchmarks

Measured on Acer Chromebook Plus 514 (Intel i3-N305, 8GB RAM, Linux container).

```
Agent overhead:  ~13 ms (DB + context build, before network)
TTFB breakdown (OpenRouter, DeepSeek V4 Flash):
  CClaw:         13 ms
  DNS+TCP+TLS:  113 ms
  LLM:         1073 ms (model-dependent)
```

## Documentation

- [specs/](specs/) — reference docs (schema, daemon, memory, providers, security, skills)
- [AGENTS.md](AGENTS.md) — project ethos, coding conventions, build instructions

## LOC

![Lines of Code](./docs/loc_chart.svg)

## Acknowledgements

### Vendored Libraries

| Library | License |
|---------|---------|
| [SQLite 3.53.1](https://sqlite.org) | Public Domain |
| [civetweb 1.16](https://github.com/civetweb/civetweb) | MIT |
| [QuickJS](https://bellard.org/quickjs/) | MIT |
| [Monocypher 4.0.2](https://monocypher.org) | BSD-2-Clause / CC0-1.0 |

### Inspiration

| Project | What we learned |
|---------|-----------------|
| Pi agent | Clean agent loop, session tree model |
| OpenClaw | Autonomy features, security patterns, multi-channel integration |
| nullclaw | Architecture reference (Zig clone of OpenClaw) |
| Letta | Stateful agent design, persistent memory patterns |
| IronClaw | Secure execution model, secret injection |

Special thanks to Mario Zechner and Peter Steinberger for starting the Claw movement, Igor Somov for showing what's possible in less than 1MB with NullClaw, and Rhett Creighton for insisting that I vibe code in C.
