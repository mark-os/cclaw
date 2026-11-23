# Architecture

CClaw is one binary and one SQLite file. The binary plays every role — the
agent loop, the sandboxed tool runner, the channel runner — and the database
(`~/.cclaw/cclaw.db`, WAL mode) holds all durable state: sessions, entries,
config, memory, secrets and every queue. This page explains how the pieces fit
together; the specs it links to hold the detail.

**CLI mode** (default) is one process: the main loop, the LLM worker threads,
and tool children. **Daemon mode** (`--daemon`) is the same process plus channel
runners, cron, the web server (dashboard, webhooks) and many agents at once.
Sub-agents are sessions inside that one process, not processes of their own.

## Processes and threads

```
cclaw                                 (CLI or --daemon)
├─ main thread: the poll() loop — the only thread that advances a session
├─ LLM worker threads                 1..4 by default (CCLAW_LLM_THREADS); each owns a
│                                     sqlite3* and a CURL*; idle ones exit after 30 s,
│                                     the last one stays
├─ tool threads                       one detached thread per database-only tool call
├─ LLM() bridge thread                one per JS run (not under `restricted`)
├─ civetweb threads                   daemon: 4 workers sharing one locked DB handle
│
├─ fork+exec → cclaw --run-tool       the broker: one per sandboxed call, scheduled
│    │                                script or hook; no key, no database
│    ├─ (file tools run right here, inside the sandbox, and stop)
│    └─ fork → sandboxed child        web, JS, shell: namespaces, rlimits, then
│         ├─ net_shim                 the loopback proxy (not under `restricted`)
│         └─ shell only: fork → PID 1 of a new PID namespace → exec /bin/sh …
└─ fork+exec → cclaw --channel <n>    daemon: one long-lived runner per channel
```

So a `shell_exec` call shows up as two `cclaw --run-tool` processes (the broker
and the sandboxed child waiting on its PID-namespace init), `net_shim`, and the
program. A `js_eval` or `web_fetch` call is the same minus the program: the
second `cclaw --run-tool` runs QuickJS or curl itself. A file tool is a single
process. Why every child execs first, and the tier table, are in
[specs/tool-execution-model.md](../specs/tool-execution-model.md).

Channel runners are trusted code (a promoted extension's JavaScript) and are
not sandboxed; they make their own HTTP and WebSocket connections.

## The main loop

The heart of the program is one plain loop on the main thread
(`src/main.c`, one for the daemon and one for the CLI). It calls `poll()` on a
handful of file descriptors:

| fd | Daemon | CLI | Readable when |
|----|--------|-----|---------------|
| SIGCHLD self-pipe | yes | yes | a child exited (tool, script, hook, channel runner) |
| internal wake pipe | yes | yes | something in this process wants a session advanced (sub-agent launch or result, a dashboard approval) |
| external wake FIFO (`<db>.pipe`) | yes | — | another process wrote a row: a channel runner's inbound event, `cclaw approvals` |
| LLM worker notify pipe | yes | yes | a model request finished and its reply is in the database |
| tool-thread notify pipe | yes | yes | a database-only tool finished and its result is in the database |
| one socket per running tool child | yes | yes | the child's result frame is arriving |
| stdin | — | yes | the user typed |

`poll()` rather than epoll because there are only ever this many descriptors.
The timeout (`compute_timeout_ms`) is the nearest deadline: a child's backstop,
a channel restart, a prompt cache about to lapse, or the next periodic database
tick — at most 30 seconds (`POLL_DB_INTERVAL`). The tick does the work no event
announces: due cron jobs, crash recovery, expired approvals, inbox and outbox
pruning, a WAL checkpoint and the disk-space floor.

**No callbacks.** This is not a libuv-style event system. Nothing slow runs on
the loop; it runs elsewhere as an ordinary blocking call and reports back
through the database:

- a model request is a blocking curl call on a worker thread;
- an untrusted tool is a forked child;
- a database-only tool (memory, cron, `db_query`) is a short-lived thread.

Each writes its result to the database and then writes one session id to its
pipe. Fire and forget, then get woken.

**One step per wake.** When woken for a session the loop calls `run_advance()`,
which calls `advance_session()`: read the session's state from SQLite, decide
the single next action (send a model request, dispatch the tool calls, deliver
the reply, or nothing), do it, return. The loop holds no picture of a turn in
memory that could drift from the database, so a crash or restart resumes from
what the database says, and the same code path serves both.

Everything starts as a row in a session's inbox — a user message, a channel
message, a cron fire, a sub-agent's result — and works its way through from
there.

**Why.** It is easy to follow and to debug on a small machine: one thread
makes every decision, in order, from durable state. There is no framework to
depend on. Concurrency comes from threads and processes the kernel already
schedules, and ordering and durability come from SQLite. **The cost:** model
replies arrive whole — there is no token streaming to the terminal — and every
slow thing needs a thread or process of its own.

## How the parts talk

| Boundary | Channel | What crosses it |
|----------|---------|-----------------|
| main loop → LLM worker | in-memory queue + `llm_jobs` row | a session id; the worker reads everything else from the DB |
| LLM worker / tool thread → main loop | **DB**, then a pipe | result rows, then an 8-byte session id to wake the loop |
| main loop → `--run-tool` broker | socketpair on fd 3 | a length-prefixed flat binary request (not JSON, ≤ 32 KB): tier, pre-extracted arguments, mounts, host rules, resolved secrets; a large hook argument travels as a passed file descriptor |
| broker → main loop | same socket | one frame: status byte, hosts-contacted JSON, result text |
| broker ↔ sandboxed child | pipe + exit code | the child's stdout/stderr; the tool status as the exit code |
| sandboxed program → network | `HTTP(S)_PROXY` → `net_shim` on loopback → a Unix socket into the broker | CONNECT tunnels and plain HTTP; the broker decides each host (default deny) and dials |
| JS `LLM()` → parent | per-call Unix socket (`.llm.<pid>.<n>.sock` in the agent dir) | 4-byte length + JSON request and reply; a parent thread runs the completion |
| channel runner → daemon | **DB** (`channel_events`) + external wake FIFO | inbound messages |
| daemon → channel runner | **DB** (`channel_outbox`) + the runner's FIFO (`<db>.<channel>.pipe`) | replies to send |
| webhook → channel runner | civetweb thread → the runner's Unix socket (`<db>.<channel>.sock`) | the HTTP request envelope; `status\nbody` back |
| `cclaw approvals`, dashboard → loop | **DB** (`approval_decisions`), then FIFO or wake pipe | a decision; the periodic tick applies it if the nudge is missed |

The rule of thumb: when a process or thread needs the loop to *act*, it writes
a row and pokes a descriptor. Descriptors carry wake-ups and the sandbox's
request and result; the database carries state.

## Who touches the database

| Who | Handle | Writes |
|-----|--------|--------|
| main loop | its own, private (`proc_db()`) | everything about turns; the only caller of `advance_session()` |
| each LLM worker thread | its own | the model's reply, `llm_responses` |
| each tool thread | its own, opened per call | the tool result entry |
| each LLM() bridge thread | its own | the completion's `llm_responses` row |
| web server | one, shared by its threads under a mutex | dashboard edits, approval decisions (as queue rows) |
| each channel runner | its own | `channel_events`, outbox status |
| CLI verbs (`approvals`, `secret`, `backup`, `sqlite3` …) | their own, short-lived | what the verb does |
| `--run-tool` broker and sandboxed children | **none** | **nothing** |

A sandboxed tool child never opens `cclaw.db`. `--run-tool` is intercepted at
the very top of `main()`, before any database, config or key setup; the child
closes every inherited descriptor above fd 3 and refuses to run if the
encryption key is loaded. Under the `standard` and `restricted` profiles the
database, its `-wal`/`-shm` files and `.cclaw_key` are also masked inside the
sandbox even if a path grant would otherwise expose them. The one exception is
the `host` profile, which has no sandbox: a shell command there runs as the
operator's user and could open any file that user can.

Every handle memory-maps the database (up to 64 MB of it — a ceiling, not an
allocation) in WAL mode, so readers do not block the writer and every thread
and process reads the same cached pages. Writers take `BEGIN IMMEDIATE`;
contention waits in a logged busy handler for up to about 4 seconds.

## Turn lifecycle

A **turn** runs from consuming queued input to delivering the final assistant
message. Within it, each model request and the tool calls it asks for make one
**iteration**.

```
1. Input arrives (channel, cron, webhook, sub-agent result, CLI stdin) → inbox row
2. The loop is woken and calls advance_session(session):
   - DISPATCH_LLM   → queue a job for the worker pool
   - DISPATCH_TOOLS → run each call inline, on a tool thread, or in a --run-tool child
   - DONE           → deliver the reply
3. A worker or tool finishes → writes to the DB → wakes the loop → step 2 again
4. Repeat until the model stops with no pending tool calls
```

Mid-turn, only model output, the results of this turn's tool calls, and a
hook's or trim's session edit between iterations reach the conversation.
Everything else waits in the inbox for the next turn boundary. See
[specs/execution-model.md](../specs/execution-model.md) for delivery modes,
background jobs and caps.

## LLM request pipeline

1. **Plan pass** (index only): a recursive CTE walks `parent_id` from the
   session's leaf to the root, collecting entry metadata without content.
2. **Cut**: walk back from the leaf adding token estimates until the budget
   runs out, snapping to the start of a turn so a turn is never split.
3. **Payload pass**: the surviving entries are joined once into a
   provider-neutral transcript, and each wire format is a
   `json_object()`/`json_group_array()` projection of it. The body points into
   the SQLite statement buffer until it is sent.

The prefix stays byte-stable for prompt caching: the payload is deterministic,
conversations grow by appending, and the per-turn context block (clock, recall,
live state) is computed once per turn and placed just before the newest turn.
See [specs/memory.md](../specs/memory.md) and
[specs/providers.md](../specs/providers.md).

## Session states

```
idle ──[inbox has work]──→ llm_running ──[tool_calls]──→ tool_running
  ↑                          ↑     │                          │
  │                          └─────┼────[all tools done]──────┘
  └──────[stop, no tools]──────────┘
```

Other states: `awaiting_approval` (a call parked for a human), `rate_limited`,
and `settling` (waiting for a hook script; [specs/hooks.md](../specs/hooks.md)).

## Security layers

| Layer | Protects | Enforced by |
|-------|----------|-------------|
| Namespace sandbox | Filesystem, network, process view | Kernel (tool children) |
| setrlimit | Fork bombs, CPU runaways | Kernel (tool children) |
| Egress proxy | Host allowlist for tool traffic | The broker (`host_decide()`) |
| Clean environment | Secret leakage to tool children | `--run-tool` exec with a two-variable environment |
| Secret scanner | Credentials entering the context | Tool results and user messages |
| Encrypted secrets | Disk theft | ChaCha20-Poly1305 in `cclaw.db` |

Under `host` the namespace sandbox and the proxy are both skipped. See
[specs/security.md](../specs/security.md) and
[specs/sandbox-profiles.md](../specs/sandbox-profiles.md).

## Limits you can hit

These are the defaults a user is most likely to run into. Config keys live in
the `config` table (an agent changes them with `request_config`); constants
need a rebuild.

| Limit | Default | Set by | What the agent sees |
|-------|---------|--------|---------------------|
| JavaScript heap (`js_eval`, extension tools, scheduled scripts, hooks) | 8 MB per run | `js_heap_mb` (1–512) | an error result: `out of memory — heap limit is 8MB; …` with advice to parse less |
| JavaScript heap (channel runners) | 8 MB | constant `CR_HEAP_SIZE` | nothing; the handler error goes to the log |
| Shell timeout | 30 s | `shell_timeout`, per agent `agents.shell_timeout`; per call `timeout` | `[timeout after 30s — raise with the timeout parameter]` |
| `web_fetch` / `js_eval` timeout | 60 s / 120 s | per call `timeout` | `error: tool timed out (…; raise with the timeout parameter)` |
| Per-call timeout ceiling | 600 s | constant `TOOL_TIMEOUT_MAX_SEC` | the request is clamped |
| Background job timeout | 600 s, ceiling 3600 s | per call `timeout`, `job_timeout_max` | a `job:timeout` notice next turn |
| Tool output kept inline | 64 KB or 2000 lines | constant `RUNTOOL_RESULT_MAX` | over either, the head (about 60 KB or 1998 lines) and `[truncated — … Full output: <path>]`; the file holds all of it |
| Sandboxed request size | 32 KB | constant `RUNTOOL_REQUEST_MAX` | `error: … exceeds 32KB cap` |
| Processes per sandbox | 256 (`standard`), 64 (`restricted`) | sandbox profile | `fork` fails inside the program |
| CPU time per sandbox | none (`standard`), 120 s (`restricted`) | sandbox profile | `[killed by SIGXCPU — CPU limit reached; …]` |
| Address space | not capped | — | — (`RLIMIT_AS` stops `go`/`node` starting; see sandbox-profiles.md) |
| Simultaneous connections per sandboxed call | 64 | constant `PROXY_MAX_RELAYS` | the program gets `502 Bad Gateway` |
| Running tool children | 48 | constant `CHILD_MAX` | nothing; the call waits for a slot |
| Model request | 300 s per attempt | `llm_request_timeout` | a retry, then an error entry citing `[resp #N]` |
| Iterations per turn | 25 | `max_iterations`, per agent | the turn ends with `error: iteration limit reached (25 per turn)` |
| `LLM()` calls per JS run | 25 | `llm_max_calls_per_tool` | `LLM()` throws, naming the cap |
| Sessions working at once | 10 | `session_max_concurrent` | autonomous turns wait; human messages always go through |
| Sub-agent sessions in flight | 100, nesting depth 2 | `session_max_active`, `agent_max_depth` | `launch_agent` refuses with the count |
| Tokens per hour / spend per day | 1,000,000 / $5 | `token_rate_limit`, `daily_cost_limit` | the session waits; the user is told it is queued |

There is no 64 MB limit on a sandbox. The 64 MB figure in the code is the
database memory-map ceiling above, and the sandbox never opens the database.

## First run

1. `cclaw` finds no `~/.cclaw/`: it creates it and `.cclaw_key` (mode 0600).
2. It creates `cclaw.db` from the schema and seeds providers, models and the
   config registry.
3. The API key is read from the environment or the encrypted `secrets` table —
   never copied into plain config. With none, it asks for one and stores it
   encrypted.
4. It creates the default agent `Assistant` and its workspace, and the user is
   chatting immediately.
