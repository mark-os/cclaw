# FUTURE — Deferred Ideas

Parking lot for features explicitly out-of-scope right now.

## Auto-Recall: Diversity + Per-Hit Metadata
- Cap hits per source session (1–2) so one session can't fill all recall slots
  (GROUP BY session_id, keep best-ranked per session)
- Tag each hit with session date + first-message snippet so the agent can judge
  whether to expand: `[session 6, 2026-06-10, "good evening assistant."] ...`
- Pairs with the existing "query entries by session_id" hint in the recall header

## Auto-Recall: Recency Tiebreak
- BM25 has no concept of time; an old session outranks yesterday's at equal score
- Blend rank with age: `ORDER BY rank + (age_days * 0.05)` or similar small
  additive penalty — bias toward recent sessions when relevance is close
- Keep pure BM25 for large score gaps (genuinely better match wins regardless)

## Multi-Speaker Sessions (per-speaker authority)
Multiple speakers already share one session in group chats, and sender
attribution exists (F19 message prefix). Unsolved: per-speaker *authority*
within a session — the tool_filter is per-session, so it can't distinguish an
admin from a stranger speaking into the same chat. Current answer: the
route's tool_filter is the floor for **all** speakers on the session, plus
admin-gated approvals for anything above it. A real fix needs per-turn
authority derived from the speaker, which conflicts with frozen-at-spawn
simplicity — parked.

## WhatsApp Business API Channel
- Webhook endpoint via civetweb: `POST /webhook/whatsapp`
- Verify endpoint: `GET /webhook/whatsapp?hub.verify_token=...`
- Send via Graph API (`POST https://graph.facebook.com/v21.0/{phone_id}/messages`)
- phone_number → session_id mapping

## Web Dashboard (Full)
Shipped: server-rendered session/agent/model/channel/grant/approval views
(`dashboard.c`, token-gated, plain HTML — no JS framework). Still future:
- Message/conversation viewer
- Web-based chat interface
- Sub-agent status + cron job views

## Branching UI
- Visual tree in web interface w/ user-selectable branching
- Optional summarization on branch navigate (Pi model: summarize abandoned branch)
- Pi pattern: `collectEntriesForBranchSummary` → find common ancestor → summarize divergent path
- Branch summary stored as entry type in session tree

## Session Curation Agent
- Autonomous agent that periodically performs branching/compaction for other agents
- Summarizes old branches, prunes dead ends
- Runs as sub-agent on schedule (cron)

## Cross-compile for Legacy Embedded Targets
armel dynamic cross-compile works in practice (Pogoplug V4 runs it, via
env-var `CC`/`BUILDDIR` overrides). Still future:
- A declared Makefile cross target instead of ad-hoc env overrides
- musl static link option
- Reduced feature set for 128MB targets (build without QuickJS? smaller JS heap)

## Multi-model Routing
Per-agent routing shipped (`agents.primary_model`/`secondary_model`, fallback
in `llm_proc.c`). Still future:
- Per-session model override stored in DB

## Additional Provider Wire Formats (Responses API, Gemini Interactions)

Planned, not started. Both had premature adapters in `db_request.c` (deleted
2026-07-13 — superseded architecture, never reachable; recover the research
via `git log -S db_build_request_typed`, but a fresh build in `llm_payload.c`
is less work than porting). Live formats today: OpenAI chat completions +
Gemini generateContent (`endpoint_type` in `llm_proc.c`).

- **OpenRouter Responses API** (`/api/v1/responses`, flat input items):
  stateless, so mostly a new payload query shaping `entries` into the item
  list + response-ingest mapping. New `endpoint_type`, no schema change.
- **Gemini Interactions API** (stateful delta sync): send only entries since
  the last sync instead of the whole branch each turn — biggest win on long
  sessions over slow links (Pogoplug class). Needs a per-session sync cursor
  column back (the v32-dropped `last_synced_entry_id`, reborn with an actual
  reader), plus desync/expiry handling (cursor invalid → full resend).
- **Anthropic Messages API** (native `endpoint_type`): we won't support
  Anthropic's OpenAI-compat `/v1/chat/completions` layer (explicitly
  non-production: no prompt caching, `strict`/`response_format` silently
  ignored, system-message hoisting, no thinking output). Native Messages is
  the only path; once that adapter lands, an `anthropic` provider row
  (`ANTHROPIC_API_KEY`) joins seed.sql.
- **OAuth provider auth**: the reference projects authenticate several
  providers via OAuth rather than static keys — Hermes: Nous Portal
  (device-code flow), OpenAI Codex / xAI / Qwen (external redirect),
  MiniMax; nullclaw: qwen-portal. cclaw needs a device-code flow first
  (fits a headless daemon: print code + URL, poll token endpoint, store
  refresh token encrypted in `secrets`), **starting with OpenAI**, which
  supports device-based auth. Token refresh belongs in the credential
  layer next to `db_secret_get_system`, not in per-provider C.
- Related: **reasoning replay** (TODO.md) — replaying stored `reasoning`
  entries in tool loops (DeepSeek R1 `reasoning_content`, Anthropic thinking
  blocks) belongs in `llm_payload.c` as a per-model capability flag.

## Search & Long-term Memory

### search_history tool
- Dedicated tool wrapping FTS5 search over agent's own sessions
- Higher-level than raw `db_query` — returns formatted results with context
- Search across all sessions, not just current

### Cross-agent search
- Agent can search other agents' sessions (with permission)
- Use case: coordinator agent reviewing sub-agent work
- All sessions already share one cclaw.db (scoped by agent_name) — needs a
  daemon-mediated tool that relaxes the scope, not new storage

### Long-term memory ideas
- Semantic search over past conversations (embedding-based, not just keyword FTS5)
- Auto-summarization of old sessions into memory blocks
- "What do I know about X?" tool that searches memory + history
- Memory consolidation agent (background, merges/prunes memory blocks over time)
- Episodic memory: key events/decisions tagged and retrievable by topic

### Shared memory blocks
- Problem: memory_blocks are scoped by agent_name in cclaw.db — no cross-agent visibility
- Approach: `shared_memory_blocks` table in cclaw.db (daemon-owned)
- Read: agents get read-only access via `shared_memory_read` tool (or bind-mount cclaw.db ro)
- Write: mediated by daemon — agent requests write via exit code 4 or a dedicated tool that posts to inbox
- Use cases: shared knowledge base, project context, team conventions, coordination state
- Scoping: optional namespace/tag per block so agents can subscribe to relevant shared memory only

## JS Extension System

**Largely built** — see [extensions.md](extensions.md) (manifest-declared tools/hooks/channel/scripts/skills/config, draft → promote → publish → attach lifecycle) and [skills.md](skills.md). Still future:

- Tool composition: JS tools calling other tools via `callTool(name, args)`
- Workspace script auto-discovery (`workspace/tools/*.sh`)
- Extension registry/marketplace: see "Extension Packaging & Community
  Registry" below

### fetch() (foundation ✓)

Synchronous fetch via `HTTP_PROXY` → net_shim loopback bridge, shipped with
host grants enforced at the proxy and a 2MB response cap
(`js_http_fetch.c`). Remaining:
- Configurable timeout per request (hardcoded 30s today)
- Streaming for large responses

## JS Runtime Capabilities

### Heap Size

4MB default for `js_eval` shipped (raised from 1MB after pogoplug session).
Still future:
- Make heap configurable per-agent
- Legacy ARM targets (128MB RAM): keep it tight, maybe 2MB max

### JSON.query — jsmn-backed field extraction

`JSON.parse` materializes the entire object tree in the JS heap (~3-4x source
size). For large API responses where the agent only needs a few fields, this
wastes the entire heap budget on throw-away data.

Proposed: expose a `json_query(source, path)` global backed by the vendored jsmn
tokenizer. jsmn tokenizes the source string in-place with a flat token array
(outside the JS heap), walks to the requested JSON path, and returns only the
extracted value as a JS string or parsed primitive.

```javascript
// source can be a string or a file path
var name = json_query(bigJsonString, "events.0.name");       // → "Egypt at Argentina"
var count = json_query(bigJsonString, "events.#");           // → 2 (array length)
var slice = json_query(bigJsonString, "events.0");           // → JSON substring (as string)
```

Memory model:
- Source string: already in JS heap (from `fs.readFile` or `http_request().body`)
- jsmn token array: C malloc, outside JS heap (~16 bytes × num_tokens)
- Result: only the extracted substring enters the JS heap

This lets an agent extract fields from a 1.5MB response without blowing the 4MB
heap — only the source string + small result are in the JS budget.

Alternative: file-path mode that reads + tokenizes without ever loading the full
content into JS heap:
```javascript
var name = json_query_file("/path/to/big.json", "events.0.name");
```

## Self-Reflection / Introspection

### DB Access from JS

The agent's SQLite DB contains sessions, messages, tool results, sub-agent state. A JS tool with DB read access enables:

- **Self-reflection**: query own conversation history, count tokens used, review past decisions
- **Cross-session search**: FTS5 search over all sessions ("when did I last discuss X?")
- **Sub-agent coordination**: check status of spawned agents without `check_agent` tool
- **Analytics**: token usage over time, tool call frequency, error rates

Implementation: inject `db.query(sql)` global that runs read-only queries against cclaw.db.

- Read-only (no INSERT/UPDATE/DELETE from JS)
- Or: separate `db.read(sql)` and `db.write(sql)` with write restricted to agent's own tables
- Result as array of objects (JSON-friendly)

### Agent State Globals

Inject read-only context about the current agent:

```javascript
agent.id          // current agent ID
agent.session_id  // current session
agent.workspace   // workspace path
agent.model       // model name
agent.parent_id   // parent agent (if sub-agent)
agent.depth       // sub-agent depth (0 = root)
```

Enables JS tools that behave differently based on context (e.g., a tool that's more conservative at depth > 0).

## HTTP Transport Abstraction

Extract curl out of the agent process behind a swappable `HttpTransport` interface:

```c
typedef struct {
    int (*request)(const char *url, const char *method,
                   const char *headers, const char *body,
                   char **response, void *ctx);
    void *ctx;
} HttpTransport;
```

Implementations:
- `http_transport_curl()` — direct libcurl (current, for standalone/testing/beefy hardware)
- `http_transport_uds(fd)` — parent holds warm TLS connections, child talks plaintext over UDS
- `http_transport_wasm()` — calls Workers `fetch()` host import for Cloudflare WASM target

Benefits:
- Pogoplug: eliminates per-turn TLS handshake (~800ms on ARMv5TE) — parent keeps connection warm
- Cloudflare Workers: agent compiles to WASM without libcurl dependency
- Same agent binary, different transport selected at startup via `CCLAW_HTTP_TRANSPORT` env

Pattern mirrors existing shell networking proxy (shell→agent UDS) but one level up (agent→parent UDS).

## Alternative Proxy Mechanisms for Static/Go/Rust Binaries

Two mechanisms shipped: `LD_PRELOAD=libcclaw_net.so` intercepts libc
`connect()`/`getaddrinfo()` for dynamically-linked programs, and `net_shim.c`
runs a loopback HTTP CONNECT listener *inside* the netns (reached via
`HTTP_PROXY` env) for static/Go/Rust binaries that honor proxy env vars.
Anything that ignores both gets zero network (`CLONE_NEWNET` hard backstop).
Still future:

### seccomp-unotify (kernel ≥5.9)
Intercept the `connect()` syscall in the parent process via seccomp user
notification (`SECCOMP_RET_USER_NOTIF`). The parent reads the target address
from the child's memory, performs the proxy logic, and injects the connected
fd back. Works for everything regardless of linking — no .so, no proxy-env
cooperation needed. Requires kernel ≥5.9 and is significantly more complex
(~500 LOC). Currently listed as a non-goal in specs/sandbox-profiles.md.

### Helper-script wrapper
Provide a `cclaw-fetch` binary in `/bin/` inside the namespace that speaks
the proxy UDS protocol directly. The LLM is instructed to use `cclaw-fetch`
instead of `curl`/`wget`. Anything not using the wrapper gets zero network
(kernel-enforced). Simple but requires LLM cooperation and doesn't help tools
like `git clone` that internally resolve + connect.

## Session Sweeper

Writes summary entries as new nodes in the parent/leaf tree (git-squash style),
never overwrites existing entries. Summaries reference the head entry-id of the
branch they summarize. Dead ends preserved as structured negative results,
invisible to context assembly, fully indexed for recall. Runs on heartbeat tick
outside the recency window.

## sqlite-vec + Binary Quantization

After FTS5 is shipping. 384-dim vectors, brute-force Hamming over
binary-quantized index, full-precision rerank of top-N, RRF fusion with BM25 and
recency term. Embedding at write time via cheap API (async, `needs_embedding`
flag, broker sweep). FTS5-only remains the offline fallback. Guide users through
local-model setup when they want it.

## Agent-to-Agent Escalation Approval (incl. multi-model councils)

Today every escalation resolves to a *human* (channel admin or CLI approver).
Idea: let a designated agent — or a quorum of agents — resolve another agent's
escalation, so autonomy doesn't always block on a person.

- **Delegated approver**: an operator names an agent as the approver for a
  scoped class of requests (e.g. a "supervisor" agent approves host grants
  for hosts on a pre-set list). The requesting agent parks as usual; the
  approver receives the parked request via inbox and resolves it.
- **Multi-model council**: instead of one approver, a quorum of N agents backed
  by *deliberately different models/providers* votes; the escalation resolves
  only on consensus (or supermajority). Model diversity is the point — it
  decorrelates prompt injection, so one poisoned council member can't grant.

**Hard tension to resolve first — do not build past this without a design
that answers it.** `specs/trust.md` states the rule plainly: *an injectable
LLM must not hand out permissions* — monitor/approver agents may **veto or
escalate, never grant**. Direct agent-approves-agent granting violates that
rule as written. The council is the interesting escape hatch (diversity +
quorum raises the bar against injection), but it is still LLMs conferring
authority, so it needs: a bounded grant surface no council can exceed (a
ceiling the operator sets, never widenable by vote); coerce-to-ONCE for
anything sensitive (rule 1 in trust.md still applies per-call); an
attributable audit trail of who voted how; and an honest statement that this
defends against injection, not against a subverted binary. Until that design
exists, the human stays in the loop for grants; councils may at most
pre-screen and *recommend*.

## LLM-in-the-Loop Evaluation (self-augmentation actually works)

The static reviews (code/security/UB) can't answer the question the whole
project rests on: can a *real model* drive draft → promote → use end-to-end,
create a bespoke agent + tools on request, and recover when a tool call fails —
without hand-holding? That needs an eval harness against live models
(`make test-e2e`-tier), not a code read. Deferred deliberately, but named so it
isn't a silent omission: build a scenario suite (define-a-tool, spin-up-a-
bespoke-agent, self-configure-a-grant) run against the default model plus a
couple of cheaper ones, scored on task completion and number of stuck turns.
The tool schemas and system prompts are the real thing under test; treat
low scores as a prompt/schema bug, not a model excuse.

## Channel Chat Commands (session + ops levers in the chat itself)

Shipped 2026-07-19 (route-model unification, v33): `/new` (fresh session,
pin re-pointed) and `/sessions [id]` (list / attach) — handled in C in
`channel_consume_events` before dispatch, admin-only, working even when the
pinned session is wedged. Still future, channel-JS side (`channel_telegram.qjs`
COMMANDS table is the dispatch point): `/status` (model, session id, pending
work), `/usage` (see Usage Visibility below), `/model`.

## Usage Visibility (token/cost accounting has writers, no readers)

`agents.total_tokens_in/out` are updated on every LLM call
(`llm_proc.c` stats update) and `llm_responses` keeps per-call metadata, but
nothing surfaces them — no dashboard panel, no doctor line, no `/usage`.
Minimal slice: a dashboard/doctor table (per-agent totals, last-24h from
`llm_responses`) + a `/usage` chat command. Cost estimates need per-model
pricing the `models` table doesn't carry — start with raw tokens.

## Channel UX Niceties (studied in reference/telegram-notes.md, not adopted)

Edit-in-place streaming preview (`editMessageText`), ack reaction (👀) on
receipt, `disable_notification` for non-final messages, typing indicator
cadence, text batching of rapid-fire messages. All are per-channel JS
concerns (no C changes) and belong in the telegram extension when they earn
their place; the outbox/`channel.http` machinery already supports them.
Message chunking (4096-byte split) is already implemented.

## Message & Turn Timestamps (optional — review-1 F20, 2026-07-12)

No entry — channel, cron, CLI, or assistant — carries a timestamp into the
LLM request (`llm_payload.c`'s `SQL_OPENAI_MESSAGES`/`SQL_GEMINI_CONTENTS`
project only `role`+`content`; `entries.created_at` exists but is never read
into the payload). Raised as "does the agent know what time it is between
messages," but kiro's research into `reference/openclaw` and
`reference/hermes` found neither reference project does full per-message
timestamping either — so treat this as a possible future enhancement, not a
parity gap:

- **OpenClaw**: `HistoryEntry.timestamp` (`src/auto-reply/reply/history.types.ts:4`)
  is metadata on the entry object; whether it becomes visible text depends on
  each channel plugin's `formatEntry` callback (`history.ts:363`) — no default
  formatter injects a visible timestamp. Interactive system prompts carry no
  date/time at all (the model calls a `session_status` tool on demand,
  `system-prompt.ts:~1123`); the exception is cron/heartbeat-triggered turns,
  which get a rich line via `resolveCronStyleNow()`
  (`agents/current-time.ts:22-30`): `"Current time: Wednesday, January 22nd,
  2025 - 14:32 (America/New_York)\nReference UTC: ..."`.
- **Hermes**: no per-message timestamps; multi-user shared sessions get a
  `[SenderName]` prefix only (no time). System prompt gets a date-only line,
  injected once per session and deliberately never per-turn
  (`agent/system_prompt.py:383-390`, comment: "Date-only (not minute-
  precision) so the system prompt is byte-stable for the full day").
- **Neither** project surfaces elapsed time between messages ("3 hours since
  your last reply").

If this gets built, the shape most consistent with both references: don't
stamp every message (sender-name extraction into plain text already landed
with the channel-routing-contract envelope fix, 2026-07-13); consider a
cron/heartbeat-turn-only rich time line (mirroring OpenClaw) rather than
adding time to every interactive turn — see `dynamic-system-prompt`'s
system-prompt-vs-additional-context split for why time-of-day can't just go
in the static system prompt.

## Extension Packaging & Community Registry (the ecosystem bet)

The long game: CClaw doesn't have to get every skill, tool, or prompt right
the first time — the platform wins if *other people* can improve it over time.
OpenClaw and Hermes are great in large part because of their communities
building skills and tools. The extension manifest already bundles the right
unit — tools + hooks + channel + scripts + skills + config + an agent to run
them all — so the missing piece is **distribution**, not a new format.

- **Package = extension bundle.** One directory + `extension.json`, exactly
  what `extension_promote` ingests today. No parallel package format; if the
  manifest can't express something a package needs (version, author,
  dependency on another extension), extend the manifest.
- **Registry**: npm-like distribution, or something better — at minimum
  install-from-URL/git with a pinned ref; ideally something fun and
  community-oriented (curated index, social discovery) rather than another
  faceless tarball server. Keep the client dumb per the ethos: fetch → verify
  → drop into `workspace/extensions/` as a **draft**. The existing
  draft → promote lifecycle is the install flow *and* the trust gate — a
  downloaded package gets zero authority until promotion approval, same as an
  agent-written draft.
- **Trust**: third-party authors are hostile until proven otherwise. The
  promotion gate, sandbox profiles, and grants already model exactly this; a
  registry adds provenance (checksums, maybe signing) on top, not a new trust
  system. Skills are the lowest-risk contribution tier (prose the model
  reads), JS tools/channels the highest — the tiering should be visible at
  install time.
- **Versioning**: updates re-enter as drafts and re-pass promotion. No
  auto-update — an unattended personal agent must not have its tools swapped
  under it by a remote publish.

Prompt-surface quality (system prompts, tool descriptions, skills) is the
cheapest layer for community contribution — which is another reason to treat
prompts and skills as first-class reviewable artifacts, not polish.
## Autonomy Ladder / Commitments (review-6 F10 — extension-bundle candidate)

OpenClaw's standout daily-driver UX: users dial autonomy up incrementally —
heartbeat (notice) → commitments (inferred, capped follow-ups the agent
volunteers and delivers on later ticks) → standing orders → cron (own a
recurring program). CClaw has both ends; the middle tier composes from
primitives: one `commitments` table + a QJS extension that extracts/caps
follow-ups from conversation + heartbeat delivery. Whether it needs even that
much — vs a skill or heartbeat-prompt convention over memory — is an open
research question (`plan/projects/commitments-research.md`); the table+QJS
sketch above is one candidate answer, not the plan.

## Seed Memories in Extension Bundles + Migration Importers (review-6, 2026-07-19)

Extend `extension.json` with an optional seed-memories payload (inline array or
a bundle file the manifest points at). `extension_promote` ingests it into the
memory table alongside the tools/channels rows — after promotion the DB is
still the one home; the file was just the installer payload. This inherits the
draft→promote trust boundary for free: seed prose (which lands in the context
window) can only enter through the approval gate, closing the
bundle-injects-persona path by construction.

This is also the substrate for importing from other systems (OpenClaw
SOUL.md/MEMORY.md, Hermes MEMORY.md/USER.md): an importer is a script that
reads their files and emits a CClaw extension bundle — no importer machinery
in core, and migrations go through the same reviewed gate as any extension.
