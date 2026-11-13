# CClaw

A minimal autonomous AI agent in C. One binary, one SQLite database file, and a sandbox around everything the agent runs. It is built for the smallest Linux machines you have: a release binary is fully static and runs on hardware as small as a 128 MB ARMv5 plug computer.

## Quick Start

From a release binary — download the one for your machine (`cclaw-linux-x64`, `cclaw-linux-arm64`, `cclaw-linux-i586` or `cclaw-linux-armv5te`) from the [releases page](https://github.com/mark-os/cclaw/releases):

```bash
chmod +x cclaw-linux-arm64 && mv cclaw-linux-arm64 cclaw
export OPENROUTER_API_KEY="sk-or-v1-..."
./cclaw
```

From source:

```bash
export OPENROUTER_API_KEY="sk-or-v1-..."
make
./build/cclaw
```

The first run creates `~/.cclaw/` and a default agent named `Assistant`, and puts you straight into a chat. `cclaw --doctor` tells you what this host supports and what is missing.

## Architecture

The core is an agent loop built on SQLite. All state lives in one database file (`cclaw.db`): sessions, messages, config, memory, secrets, queues. Each step of a turn reads that state and decides what to do next. Everything else is a layer on top of the core, with a clear boundary:

| Layer | What it adds | What it needs from the host |
|-------|--------------|-----------------------------|
| Core | Agent loop, SQLite, config and self-reconfiguration, LLM requests | A kernel, a filesystem, a CA bundle for HTTPS |
| Tools | File tools, `web_fetch`, JavaScript (`js_eval`, extension tools) | Nothing more |
| Containment | A sandbox around code cclaw cannot inspect | Linux user namespaces today |
| Shell | Arbitrary programs (`shell_exec`) | Containment, or the operator's explicit `host` opt-out |
| Daemon | Channels, cron, webhooks, multi-agent | `fork`/`exec` of the cclaw binary, signals |

A host that lacks what a layer needs loses that layer and nothing below it. (As built today, the tools layer still leans on containment too: every tool call, not just the shell, runs in the sandbox.) [specs/portability.md](specs/portability.md) describes the layers and where they are heading.

The process uses threads for concurrency and fork+exec for isolation. The main thread runs a poll loop — no callbacks, no async. LLM calls are blocking HTTP requests on worker threads. Each sandboxed tool call runs in its own short-lived child process.

```
Main process
├─ main thread: poll loop (stdin, wake fd, signals, worker notifications,
│                periodic DB tick for cron and recovery)
├─ LLM worker threads (1–N elastic, each owns sqlite3* + CURL*)
│    blocking HTTP calls, connection reuse, 30s idle timeout
├─ tool threads (DB-only tools: one short-lived thread per call)
│
├─ fork+exec → cclaw --run-tool   (one per sandboxed tool call: file, web, JS, shell)
├─ fork+exec → cclaw --channel    (daemon: one long-lived runner per channel)
└─ civetweb threads               (daemon: webhooks, dashboard)
```

The database runs in WAL mode, so threads and processes read and write it concurrently without blocking each other.

**CLI mode** (default): one process with the worker thread pool. No daemon needed.

**Daemon mode** (`--daemon`): adds channels (Telegram, Discord, WhatsApp, webhooks), cron, the web dashboard, and many agents working at once.

## Secure

What an agent can reach is decided in two places: its **sandbox profile** (containment — what the sandbox lets a program touch) and its **grants** (authority — which tools, hosts and paths it may use). A missing grant means no. Agents ask for more with `request_config`, and a human approves.

- **Sandboxed tool calls** — every tool call runs in a forked child inside Linux namespaces (user, mount, network). The child sees its workspace read-write and system directories read-only; anything else needs a path grant. `cclaw.db` and `.cclaw_key` are hidden from the child under every sandboxed profile.
- **Network** — the sandbox has only a loopback interface. Programs reach the network through `HTTP_PROXY`/`HTTPS_PROXY`, which point at a proxy on that loopback. The proxy checks the agent's host allowlist (default-deny); a denied host gets a `403`, and the tool result names the blocked host. Programs that ignore the proxy variables have no network at all. `git` over ssh is preconfigured; plain `ssh` needs `-o ProxyCommand="$CCLAW_CONNECT %h %p"`. See [specs/shell-networking.md](specs/shell-networking.md).
- **Sandbox profiles** — `standard` (the default), `restricted` (no network, ever), and `host` (no sandbox at all, for machines that are already isolated or cannot run one). A sandboxed profile that cannot set up its sandbox refuses to run the tool rather than running it unsandboxed. See [specs/sandbox-profiles.md](specs/sandbox-profiles.md).
- **Secrets** — encrypted at rest in `cclaw.db` (ChaCha20-Poly1305, via Monocypher). The model refers to a secret as `{{SECRET:NAME}}`; cclaw substitutes the real value just before the tool runs, so the value never enters the context window or the logs. A scanner redacts credential-shaped text from tool results and user messages.
- **Resource limits** — `setrlimit` caps the process count (and, under `restricted`, CPU time) to bound a runaway. Address space is deliberately *not* capped: `RLIMIT_AS` limits reservations rather than usage, which stops `go`/`node`/`rustc` from starting without bounding real memory.

The full model is in [specs/security.md](specs/security.md) and [specs/trust.md](specs/trust.md).

## Portable

The goal is to run on the widest range of old, embedded and very cheap Linux devices, and later on platforms that bring their own sandbox, such as Android.

CClaw vendors everything except libcurl:

| Vendored | Purpose |
|----------|---------|
| SQLite 3.53 | All persistence, JSON, full-text search |
| QuickJS | JavaScript engine for agent-written tools, channels and scripts |
| civetweb | Embedded HTTP server |
| Monocypher | Encryption |
| jsmn, yxml | Small JSON and XML parsers |

**Release binaries** are fully static (musl, with a pinned libcurl and mbedTLS inside) for x64, arm64, i586 and armv5te. They need no shared libraries, only a CA certificate bundle on the host. See [specs/static-build.md](specs/static-build.md).

**A source build** (`make`) needs a C11 compiler and the libcurl development headers, and links the system `libcurl.so`. Use it for development and for architectures without a release binary (ARMv7, RISC-V, …).

Tested on EC2 t4g.small (arm64), Chromebooks, and a Pogoplug V4 (ARMv5TE, 128 MB RAM).

```
Binary size: about 4 MB stripped (x86_64 source build; SQLite and QuickJS are over half of it)
Peak RSS:    9.7 MB (single turn)
Startup:     10 ms
```

## Self-Configuring

Agents change their own configuration through tool calls. Anything that widens what an agent can do goes to a human for approval first.

- **`search_config`** — the agent's view of itself: config keys, grants, attached extensions, and the roster of other agents.
- **`request_config`** — one *changes document* covering any mix of grants, the agent's own model list, config keys, an LLM provider, models, channel send routes, and secret host bindings. One approval covers the whole document. See [specs/config-doc.md](specs/config-doc.md).
- **`create_agent` / `update_agent`** — define and refine purpose-built agents. An agent can never create one with more reach than it has itself.
- **`launch_agent`** — hand a task to another agent, or to a worker copy of itself with a narrower tool set.

Configuration resolves in layers: `CCLAW_*` env vars override DB values, which override built-in defaults. Agents read configuration freely but write it only through these tools.

## Built-in Tools

Every new agent is granted a baseline toolset; the rest it asks for with `request_config`. More tools can be added at runtime — see [Extensible](#extensible).

| Tool | Purpose | Granted by default |
|------|---------|--------------------|
| `file_read` / `file_write` | Read a file / create or overwrite it entirely | yes |
| `file_edit` | Targeted search/replace edits — batched, matched against the original, non-overlapping | yes |
| `file_list` | List a directory (sorted alphabetically, `/` marks directories) | yes |
| `file_find` | Find files by glob (`*.c`, `src/**/*.spec.ts`) | yes |
| `file_grep` | Search file contents by POSIX regex, returns `path:line:match` | yes |
| `js_eval` | Run JavaScript in the sandboxed QuickJS engine | yes |
| `memory_block_create` / `memory_block_update` / `memory_note_add` / `memory_note_edit` / `memory_note_delete` | Manage persistent memory: named blocks holding notes addressed by id | yes |
| `search_config` / `request_config` | Read configuration / ask for changes | yes |
| `cron_set` / `cron_list` / `cron_remove` | Schedule its own prompts and scripts | yes |
| `launch_agent` / `check_session` | Delegate work and check on it | yes |
| `shell_exec` | Run a shell command in the namespace sandbox | on request |
| `web_fetch` | Fetch a URL (host-allowlisted) | on request |
| `db_query` | Query the SQLite database read-only | on request |

The baseline list is the `agent_default_tools` config key. File tools are workspace-scoped; `file_find`/`file_grep` skip `.git` and `node_modules`. `js_eval` runs modern JavaScript (QuickJS — ES2023-class; no modules, no event loop, no top-level `await`, no `Atomics`) with the globals `fs.readFile/writeFile/readdir/stat/cwd`, `http_request`, `XML.parse`, and `LLM()` — see [specs/extensions.md](specs/extensions.md#js-dialect--globals).

## Extensible

Agents extend themselves at runtime, and QuickJS is how: C provides the peanut butter, JavaScript the jelly on top. An **extension** is the unit of sharing: a directory with an `extension.json` manifest declaring any mix of tools, hooks, a channel, scripts, [skills](specs/skills.md), agents and config keys, plus the JavaScript handler files it points to. The code lives in files; the database stores only definitions and paths. The lifecycle:

1. **Draft** — the agent writes the bundle in its private workspace (`workspace/extensions/<name>/`) and tests handlers with `js_eval`. Personal skills need no bundle at all — drop a `SKILL.md` in `agents/<name>/skills/`.
2. **Promote** (`extension_promote`) — validates the manifest, and after the operator approves, copies the bundle into the shared store (`~/.cclaw/extensions/<name>/`, which agents cannot modify) and registers its contents. Still visible only to the owning agent.
3. **Publish** (`extension_publish`) / **attach** (`extension_attach`) — other agents opt in. Attaching never grants authority: tools remain subject to each agent's own grants.

The manifest is the only door from an agent's drafts to system-wide registration — one choke point where everything is validated and listed before it registers. Channels are self-contained JavaScript programs run as daemon-managed processes (Telegram, Discord and WhatsApp ship built in); extension config lands in the config registry as `<ext>.<key>`. JavaScript tools can call the agent's own model with `LLM(prompt, opts)` ([specs/js-llm.md](specs/js-llm.md)). See [specs/extensions.md](specs/extensions.md) for the authoring walkthrough.

## Self-Bootstrapping

On first run with just an API key, CClaw:

1. Creates `~/.cclaw/`, the database, and the encryption key.
2. Creates the default agent, `Assistant`, with the baseline grants (plus `create_agent`, `extension_publish` and `secret_create`, since it is your main point of contact), `AGENT` and `USER` memory blocks, and a disabled heartbeat cron job.
3. Starts a chat. The agent has no name yet; it asks what you would like to call it and saves the answer to its memory.

From there you set things up by talking to it: ask for a Telegram channel, a different model, a new agent. Anything that widens its reach comes back to you as an approval. Secrets never pass through the model: put them in the environment, store them with `cclaw secret set NAME`, or have the agent capture a tool result directly into the encrypted store with `save_secret`.

No manual config files. No YAML. The agent configures itself through the same tool-call interface it uses for everything else.

## Usage

```bash
cclaw                    # interactive CLI (default agent, streaming)
cclaw -p "hello"         # single-turn: print response and exit
cclaw -s 3               # resume session 3
cclaw --new              # force new session
cclaw --daemon           # daemon mode (channels, cron, multi-agent)
cclaw install [--system] # set up the daemon as a systemd (or SysV init) service
cclaw --doctor           # redacted diagnostic report: TLS, layers, updates, …
cclaw update             # install the latest release (refuses one that can't open this DB)
cclaw rollback           # undo the last update: binary and DB snapshot together
cclaw backup [dest]      # consistent snapshot of the database, safe on a live daemon
cclaw resp               # the last failed LLM request and response
cclaw sqlite3 <db> "…"   # bundled SQLite shell, read-only unless --write
cclaw -v                 # debug logging (timing, context stats)
cclaw -vv                # trace logging (full LLM req/resp JSON)
cclaw --help             # all options
```

Updating, rollback and pre-release channels (`cclaw publish`) are described in [specs/operations.md](specs/operations.md).

## Configuration

Priority (highest first):

1. `CCLAW_*` env vars
2. `~/.cclaw/cclaw.db` `config` table (registry-backed: every key ships a default + description; DB rows are overrides)

Provider API keys are resolved separately, per provider: each `providers` row
names the variable holding its key (`api_key_env`), read from the environment
first and then from the encrypted `secrets` table. `OPENROUTER_API_KEY` works
out of the box because the seeded `openrouter` row names it — it is not a
global fallback, and a provider whose key resolves nowhere is skipped by
routing rather than being tried without credentials.

```
~/.cclaw/
├── cclaw.db           ← all state
├── .cclaw_key         ← encryption key (mode 0600)
├── extensions/        ← shared extension store
└── agents/
    └── Assistant/
        └── workspace/ ← the agent's files
```

Set `CCLAW_DB_PATH` to put the database (and everything beside it) somewhere else. See [docs/configs.md](docs/configs.md).

## Building

```bash
make              # build ./build/cclaw
make smoke        # fast unit-test subset (a few seconds)
make test         # unit tests (no network)
make test-integration  # tests against mock servers
make debug        # ASan + UBSan build (clang, -O0 -g3)
make clean        # remove build/
scripts/build_static.sh arm64   # static release binary (x64|arm64|i586|armv5te)
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

- [docs/](docs/) — architecture, configuration, debugging, templates
- [specs/](specs/) — detailed reference (schema, security, sandbox profiles, portability, providers, extensions, operations, …)
- [AGENTS.md](AGENTS.md) — project ethos, coding conventions, build instructions
- Running agents find their own documentation as built-in skills (the `cclaw-docs` extension, from `templates/docs_*.md`)

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
| [jsmn](https://github.com/zserge/jsmn) | MIT |
| [yxml](https://dev.yorhel.nl/yxml) | MIT |

### Inspiration

| Project | What we learned |
|---------|-----------------|
| Pi agent | Clean agent loop, session tree model |
| OpenClaw | Autonomy features, security patterns, multi-channel integration |
| nullclaw | Architecture reference (Zig clone of OpenClaw) |
| Letta | Stateful agent design, persistent memory patterns |
| IronClaw | Secure execution model, secret injection |
| Hermes | Self-improving agent loop, skill and extension library patterns |

Special thanks to Mario Zechner and Peter Steinberger for starting the Claw movement, Igor Somov for showing what's possible in less than 1MB with NullClaw, and Rhett Creighton for insisting that I vibe code in C.
