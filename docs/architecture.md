# Architecture

## Process Model

CClaw has two execution modes sharing the same binary:

**CLI mode** (default): single process, runs the agent loop directly. No daemon, no threads (except the credential proxy for shell children). Opens `~/.cclaw/cclaw.db` and talks to the LLM.

**Daemon mode** (`--daemon`): epoll loop that forks isolated agent processes per session turn. Manages channels (Telegram, webhooks), cron, and the web dashboard. Never executes LLM logic itself.

```
Daemon (optional)                    Agent process (forked or standalone)
├── Telegram poller thread           ├── Opens cclaw.db (shared, via CCLAW_DB)
├── Civetweb thread (webhooks)       ├── setrlimit (memory/CPU caps)
├── Cron scheduler thread            ├── Drains inbox → builds context
├── Heartbeat thread                 ├── LLM call (libcurl)
├── Epoll: signal pipe + SIGCHLD     ├── Tool dispatch loop
│                                    ├── Writes response to DB
└── Forks agent on inbox signal      └── exit(code) — signals intent
```

## Single DB

All state lives in `cclaw.db`. Agent data scoped by `agent_name` column. Sessions scoped by `session_id`. Parent (CLI/daemon) is primary writer.

See [specs/schema.md](../specs/schema.md) for full DDL.

## LLM Request Pipeline

### Two-Pass Context Building

Building an LLM request never loads the full session into memory:

1. **Plan pass** (index-only): Recursive CTE walks `parent_id` from `leaf_id` to root, collecting entry metadata (`role`, `stop_reason`, `token_estimate`, `tool_call_count`). Covered entirely by `idx_entries_plan` — no main table access, no content loaded. Result is reversed to root→leaf order.

2. **Cut**: Walk the plan array from leaf toward root, accumulating token estimates until budget is exhausted. Tool-call groups (assistant + tool_results) are kept or dropped as a unit.

3. **Payload pass**: Surviving entry IDs go into a temp `_plan` table. SQL joins `_plan` against `entries` to produce the final JSON payload via `json_object()`/`json_group_array()` — one query per provider format (OpenAI or Gemini). The result is zero-copy: `payload.body` points directly into the SQLite statement result buffer until `llm_payload_release()`.

### Why Entries Are One Row Per Piece

An assistant turn with tool calls is stored as separate entries:

```
reasoning (type='reasoning', part_index=0)
  → assistant_message (type='assistant_message', part_index=1)
    → tool_call (type='tool_call', part_index=2)
      → tool_call (type='tool_call', part_index=3)
```

Each is a node in the `parent_id` chain. This design is driven by four requirements:

**Multi-provider wire emission (V60)**: Each provider reassembles tool calls differently — OpenAI stringifies args, Gemini uses native objects in a `parts` array, Anthropic uses content blocks. Storing args as a plain `content` column per tool_call entry lets the SQL payload builder emit any format without parsing JSON.

**Streaming writes**: Tool calls arrive incrementally during SSE streaming. Each call is committed as its own entry the moment assembly completes, rather than buffering the entire turn.

**Uniform tree for planning (V56)**: The plan pass walks a linked list of entries using only integer columns. If tool calls were embedded JSON inside the assistant entry, planning would need to parse content to count or estimate them.

**Daemon dispatch (V13/V77)**: When an agent exits with code 2/3, the daemon finds the specific `tool_call` entry by `tool_call_id` to read spawn/approval args — a simple `SELECT content FROM entries WHERE tool_call_id=?`.

The cost is that OpenAI-format payload building requires correlated subqueries to re-merge tool_call entries back into the assistant message's `tool_calls` array. This runs only on entries that survived the cut (~20-50 entries), indexed by `turn_id`.

### Prompt Caching

All major providers offer prefix-based caching — if the beginning of your request matches a previous request, the cached prefix is reused at reduced cost. CClaw benefits from this automatically because:

1. The payload is deterministic (same entries → same JSON, byte-for-byte).
2. Conversations grow by appending at the end — the prefix (prior turns) is stable.
3. No per-request variation is injected into the prefix (recall text is appended after all messages).

Provider-specific behavior:

- **OpenAI/OpenRouter/Gemini**: Fully implicit. No wire format changes. Send the same prefix, get automatic savings. OpenAI caches at every 128-token boundary (min 1024 tokens), so partial prefix matches work — appending varying content at the end doesn't prevent cache hits on the stable prefix.
- **Anthropic**: Requires opt-in via `"cache_control": {"type": "ephemeral"}` on the request body. Still a single request format — not a 2-step upload-then-reference flow. The server caches at the breakpoint, charges writes at 1.25× and reads at 0.1× base cost.
- **OpenRouter sticky routing**: Routes subsequent requests to the same physical backend so the KV cache stays warm. Automatic when cache usage is detected, or force with `session_id` header.

What breaks the cache:

- **Changing tool definitions**: Tools are serialized before messages in the cache prefix (`tools → system → messages`). Any modification invalidates the entire cache. Accept the one-turn miss for rare tool changes, or keep a stable core toolset.
- **Changing the system prompt**: Same as tools — it's part of the prefix.
- **Recall text at the end**: Does NOT break the cache. It's appended after all messages, past the stable prefix. Providers cache sub-prefixes (128-token boundaries on OpenAI), so the conversation portion gets cache hits regardless of trailing content.

## Exit Code Protocol

Agent processes communicate intent to the daemon via exit codes. The daemon reads details from cclaw.db after reap.

| Code | Meaning | Daemon Action |
|------|---------|---------------|
| 0 | Turn complete | Deliver last assistant entry to channel |
| 1 | Error | Log error, mark session idle |
| 2 | Spawn sub-agent | Read tool_call args, fork child agent |
| 3 | Approval needed | Read tool_call args, notify admin |
| 4 | Config change | Read tool_call args, validate + apply |
| 127 | exec failed | Log error |
| 128+N | Killed by signal N | Log crash, synthesize error entry |

In CLI mode, exit codes are unused — the process handles everything inline.

## First-Run Flow

1. User runs `cclaw` — no `~/.cclaw/` exists
2. Create `~/.cclaw/`, generate `.cclaw_key` (32 random bytes, mode 0600)
3. Create `cclaw.db` with schema, seed default config
4. If `OPENROUTER_API_KEY` in env → encrypt and store in cclaw.db kv
5. Create default agent in cclaw.db + `agents/default/workspace/`
6. Enter agent loop — user is chatting immediately

Total first-run overhead: ~165ms (schema creation + WAL init).

## Config Injection

Daemon reads agent config from cclaw.db at fork time, injects as `CCLAW_*` env vars. Agent processes only read env vars — never open cclaw.db for config lookup (they open it for data access via `CCLAW_DB`).

See [docs/configs.md](configs.md) for the full config reference.

## Security Layers

| Layer | Protects | Enforced by |
|-------|----------|-------------|
| setrlimit | Resource exhaustion | Kernel (agent process) |
| http_check_policy | Network exfiltration | Application (agent process) |
| Namespace sandbox | Filesystem + network | Kernel (shell children) |
| UDS proxy | Host allowlist for shell | Application (agent process) |
| Env stripping | Secret leakage to shell | Application (agent process) |
| Encrypted secrets | Disk theft | ChaCha20-Poly1305 (cclaw.db) |

See [specs/security.md](../specs/security.md) for full details.
