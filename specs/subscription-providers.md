# Subscription-Backed Providers (Claude / ChatGPT) — Survey & Decision

**Status: survey of 2026-08-26, reviewed 2026-11-13. This is a snapshot, not a
roadmap. Nothing here is built. See Decision below.**

The 2026-11-13 review added three sections: [a working design of the
sanctioned route](#a-working-design-of-the-sanctioned-route-opencode-2026-10),
[the Agent SDK underneath](#the-agent-sdk-underneath) and [how cclaw could
copy the design](#how-cclaw-could-copy-the-opencode-design). It did not change
the decision. OpenClaw's simpler `claude -p` backend is described in
[reference/openclaw-claude-cli.md](../reference/openclaw-claude-cli.md).

CClaw's providers today are all API-key, curl-on-a-worker-thread (`specs/providers.md`).
This doc records what it would take to instead drive inference off a *personal
subscription* (Claude Max, ChatGPT Plus/Pro) rather than a metered API key, based on
reading how four existing agent projects do it: OpenClaw (TS), nullclaw (Zig, OpenClaw
clone), Hermes (TS/Python), and Anthropic's own Agent SDK docs.

## The vendor-sanctioned lane, per vendor

| Vendor | Sanctioned subscription mechanism | What it requires |
|---|---|---|
| Anthropic | Run the real `claude` binary (`claude -p` / Claude Code interactive). The Agent SDK is *not* a separate mechanism — both TS and Python SDKs bundle the CLI binary and spawn it, speaking `stream-json` over stdio. There is no supported path to subscription inference that doesn't involve that binary running. | Node/CLI install, logged-in `claude` (or `claude setup-token` for headless), a subprocess per session. |
| OpenAI | Run the `codex` binary (`codex exec` for scripting), or "Sign in with ChatGPT" inside a first-party surface. | Codex CLI install + login. |

Both vendors converge on the same shape: subscription auth is honored only inside
their own harness process. Neither offers a documented way to take a subscription
OAuth token and make arbitrary custom requests to their real production API.

## The unsanctioned lane: raw HTTP + impersonation

All three OSS projects surveyed also have (or had) a second route: obtain the
subscription's OAuth token directly (PKCE against `claude.ai`/`chatgpt.com`, or a
device-code flow, or by reading the vendor CLI's own credential file) and speak
raw HTTP to a backend endpoint, decorated with headers/payload that make the
request look like it came from the vendor's own CLI.

### Anthropic side — actively defended, arms-race in progress

- **OpenClaw** (`packages/ai/src/providers/anthropic.ts:1110-1131,1548-1570`):
  detects an OAuth token (`sk-ant-oat…`), sends `anthropic-beta:
  claude-code-20250219,oauth-2025-04-20`, `user-agent: claude-cli/<ver>`, `x-app:
  cli`, and prepends a spoofed first system block — literally "You are Claude
  Code, Anthropic's official CLI for Claude." Comment in the code: Anthropic uses
  that first block to route subscription billing. OpenClaw's own docs call this
  "technical compatibility only, not an officially sanctioned path." OpenClaw's
  *default*, recommended route for subscriptions is still the CLI subprocess
  (`extensions/anthropic/cli-backend.ts`) — HTTP-with-OAuth is the fallback for
  when spawning a binary isn't desirable.
- **nullclaw**: has *no* Claude OAuth login flow at all. Only a pasted
  `sk-ant-oat01-` token (`src/providers/anthropic.zig:57`) or a `claude` CLI
  spawn (`src/providers/claude_cli.zig`, 778 lines). The authors evidently didn't
  find the HTTP route worth building out.
- **Hermes** goes furthest, and is the clearest evidence of an active arms race
  (`agent/anthropic_adapter.py`):
  - Split user-agents: `axios/1.7.9` at the OAuth token endpoint (real
    `claude-code/*` UAs get HTTP 429 there — empirically discovered), vs
    `claude-code/<live-version> (external, cli)` at the inference endpoint,
    with the version scraped live from a local `claude --version` so the
    spoof doesn't go stale.
  - System-prompt string laundering: prepends the Claude Code identity block,
    then rewrites the caller's own system prompt ("Hermes Agent" →
    "Claude Code", "Nous Research" → "Anthropic") to dodge content filters.
  - **Billing-classifier evasion**: rewrites every tool name to `mcp__<name>`
    on the wire and reverses it on the response — because a single-underscore
    `mcp_foo` name flips the request from plan-billing into the extra-usage
    lane (HTTP 400 "Third-party apps now draw from extra usage, not plan
    limits").
  - Co-tenancy with the real Claude Code install: reads *and writes back*
    `~/.claude/.credentials.json` (synthesizing a `scopes` array newer Claude
    Code versions gate on), and adopts Claude Code's own rotated refresh
    token to avoid a refresh race between the two processes.
  - **Its own docs admit the payoff**: the OAuth path "only works on a Claude
    Max plan with purchased extra usage credits — the base Max allowance is
    never consumed." All that machinery still doesn't reach the plan's
    *included* usage; it reaches the overage lane.

### OpenAI side — comparatively open, still unsanctioned

- Codex CLI's OAuth token (`~/.codex/auth.json`) works directly against
  `https://chatgpt.com/backend-api/codex/responses` with just
  `Authorization: Bearer <token>` — no header/system-prompt impersonation
  needed. This is what makes it "easier": nullclaw implements it in
  `src/providers/openai_codex.zig` in ~1,400 lines (raw HTTP via curl
  subprocess for SSE streaming, Zig `std.http` only for the OAuth dance),
  with a device-code login (`auth.openai.com/oauth/device/code`, same client
  id `app_EMoamEEZ73f0CkXaXp7hrann` the real Codex CLI uses) or import from
  `~/.codex/auth.json`.
- Hermes' Codex route is built the same way but *without* impersonation —
  it identifies itself honestly as `hermes-cli/<ver>` — and it still works.
- **Known gap**: nullclaw's raw-HTTP Codex route never sends a `tools` array
  (`supportsNativeTools` returns false) — tool calling falls back to
  text-based dispatch. Whether the backend endpoint supports native tool
  calls at all is unverified from the OSS reading alone; OpenClaw's separate
  Responses-transport code suggests it might, but that would need to be
  checked empirically before it's a real option for cclaw's tool-call-native
  turn model.
- Still explicitly unsanctioned: this is the same "special backend endpoint,
  not the public API" pattern as Anthropic's, just without active
  enforcement pressure as of this writing.

## Anthropic's own billing policy churn (context for why this is unstable)

- **2026-04-04**: banned third-party agent usage on Claude subscriptions
  outright, after Max subscribers were reportedly generating
  hundreds-to-thousands of dollars of actual inference through inefficient
  agents.
- **2026-05-13/14**: reversed the ban, announced a *dedicated* monthly Agent
  SDK credit — separate from chat/Claude Code usage — covering Agent SDK,
  `claude -p`, and OAuth third-party apps: $20/mo Pro, $100/mo Max 5x, $200/mo
  Max 20x, non-rollover, effective 2026-06-15.
- **2026-06-15**: paused the credit plan *before* it took effect. Current
  Anthropic help-center policy: Agent SDK / `claude -p` / third-party usage
  still draws from ordinary subscription limits (the same pool as interactive
  Claude Code); the announced credit "isn't available"; a further update is
  pending, undated.

Net effect right now: your Claude Max plan's included limits **are** legitimately
usable by cclaw, but only through the CLI-subprocess mechanism, sharing the same
pool as interactive Claude Code sessions — no separate agent allowance exists yet.

OpenAI's parallel history is calmer: Codex moved to token-based credit overflow
on top of plan limits in April 2026 and hasn't reversed it.

## A working design of the sanctioned route (OpenCode, 2026-10)

OpenCode shipped a built-in "Claude Agent" provider in v2.2.3 (2026-10-30). It
is the first of the surveyed projects to build the sanctioned route in full:
the user's own `claude` program does the inference, and the host keeps its own
tools, permissions and history. This section records how it works, because it
answers the three open problems in the cost estimate below.

Sources: OpenCode PR #58782, its design spec
(`specs/v2/claude-agent-provider.md` in `anomalyco/opencode`), and the source
under `packages/core/src/plugin/provider/claude-agent/`. Read 2026-11-13.
Statements marked (source) were checked in the code; the rest are from their
spec.

### Shape

- OpenCode starts the user's installed `claude` through the Claude Agent SDK
  (a Node package). It finds `claude` on `PATH`, or takes the path from
  `OPENCODE_CLAUDE_PATH`.
- OpenCode does not read or store the login. The user runs `claude auth login`
  in a terminal. Any login works: a subscription, an API key, a cloud account.
- OpenCode does not sit between `claude` and Anthropic and does not change the
  requests `claude` sends.
- There is one long-running `claude` process for each session. It stops after
  5 minutes idle. The next message resumes the saved Claude session by id.

### Prompt

- Claude Code's own system prompt stays. OpenCode cannot replace it.
- OpenCode appends to it: a short note that Claude runs inside OpenCode, the
  selected agent's prompt, and OpenCode's instruction sources (AGENTS.md
  files, skills).
- OpenCode leaves out its own date and environment text, because Claude Code
  writes those.

### Tools over MCP

This is the part that keeps the host in control.

1. **Built-in tools off.** The SDK options are `tools: []`,
   `allowedTools: ["mcp__opencode__*"]`, `permissionMode: "dontAsk"` and
   `strictMcpConfig: true` (source). Claude can call the host's tools and
   nothing else. A call to any other tool gets Claude Code's own "No such tool
   available" answer, and the host never sees it.
2. **Host tools as one MCP server.** OpenCode builds an MCP server named
   `opencode` and passes it to the SDK as an in-process server
   (`type: "sdk"`), so there is no socket and no port (source). The server
   answers `tools/list` with the host's tool definitions and their JSON
   schemas. Claude sees each tool as `mcp__opencode__<name>`.
3. **A call blocks until the host answers.** The `tools/call` handler does not
   run the tool. It parks the call and waits. OpenCode then runs the tool
   through its normal path, permission prompt included, and hands the result
   back to the parked handler (source: `pending.call(name)`).
4. **Two hooks carry what MCP cannot.**
   - `PreToolUse` gives the host Claude's `tool_use_id` for the call. The MCP
     handler does not receive that id, and the host needs it to store the
     call and result as a pair.
   - `PostToolUse` replaces the tool output (`updatedMCPToolOutput`) to add
     PDFs and images, which a plain MCP result cannot carry.
5. **Calls are serial.** The host remembers one pending id at a time, so every
   tool is declared `readOnlyHint: false`. Claude Code runs such tools one at
   a time (source).
6. **Limits found the hard way** (source):
   - A tool name is at most 64 characters with the prefix, and only
     `[a-zA-Z0-9_-]`. Longer or other names are cut and given a hash suffix.
   - Claude Code cuts an MCP tool description at 2048 characters.
   - Tool search must be off (`ENABLE_TOOL_SEARCH=false`) and each tool marked
     `anthropic/alwaysLoad`, or Claude Code hides every MCP tool until the
     model searches for it.
   - `MCP_TOOL_TIMEOUT` must be unset, or it cancels a tool call that waits on
     a human.
   - `MAX_MCP_OUTPUT_TOKENS` is raised so that the host's truncation applies
     and not Claude Code's.
7. **A changed tool list restarts the process.** The SDK cannot swap tools in
   a running session.
8. **No fallback.** If the host's tools cannot be given to Claude, the request
   fails. It never falls back to Claude Code's tools.

### Everything else in Claude Code is off

The host passes every setting itself and Claude Code reads none of the user's
(`settingSources: []`, `skills: []`, `disableAllHooks`, source). Off by
environment variable: CLAUDE.md files, auto memory, auto compaction, away
summaries, the advisor tool, the non-streaming fallback and the refusal
fallback. The reason is the same each time: the host has its own version, or
the host could not show or control what Claude Code did.

Three shell variables are cleared so that they cannot override the host:
`CLAUDE_CODE_SIMPLE` (it skips the subscription login),
`CLAUDE_CODE_EFFORT_LEVEL` and `MCP_TOOL_TIMEOUT`.

`verbatimPrompts: true` sends the host's message unchanged and skips the
context Claude Code adds at the start of a turn. It needs Claude Code 2.1.248
or later.

### History

The host's history is the source of truth. Claude Code keeps its own
transcript, and it cannot load messages it did not write. OpenCode reconciles
the two:

- When a session switches to Claude, everything Claude has not seen goes in as
  one user message inside `<conversation-checkpoint>`, without thinking text.
- When it switches back later, only the missed part is sent.
- After an edit or an undo, the host copies Claude's session up to the last
  point where both histories match and continues from there. If nothing
  matches, Claude starts fresh with the whole history as a checkpoint.
- Earlier thinking is dropped when the prompt or tools change mid-session.
  Each thinking block is tied to the prompt and tools it was made with, and
  the API rejects a replay that does not match.

This sync is the largest part of their change (a `sync/` directory of five
files, plus session copy and fork code).

### Accounting

- Claude Code retries failed requests itself. The host does not retry.
- A subscription login shows a cost of 0.
- Claude Code reports usage for one response several times while it streams,
  and again as a turn total. The host keeps the highest count for each
  response. A plain sum counts a response several times.

### What they could not keep

- Sampling settings: temperature, top-p, stop sequences, seed.
- Their own base prompt for built-in agents.
- HTTP request and response hooks, because Claude Code makes the API calls.
- Parallel tool calls.
- Hosted and remote use: `claude` must be installed and signed in on the same
  machine.

### Policy position (Anthropic's pages, read 2026-11-13)

- Not allowed: a third-party program that calls the API with a subscriber's
  OAuth token, and "log in with Claude" in a third-party product. "Anthropic
  does not permit third-party developers to offer Claude.ai login into their
  own applications, or to route requests through Free, Pro, Max, or Ultra plan
  credentials on behalf of their users."
- Allowed: the rules do not "prevent an end user from signing in to the
  unmodified Claude Code binary with their own Claude subscription."
- Not settled: billing. A support article says Anthropic "reserves the right
  to draw use of such third-party tools from usage credits rather than
  subscription limits." No page says whether a host that starts `claude` is
  such a tool.

OpenCode's design sits in the allowed case: the user signs in to the
unmodified binary, and the host only drives it. Its spec lists two things it
did not do because Anthropic's documents forbid them: use the name "Claude
Code" for the provider, and reuse the credentials to read plan usage.

### What this changes in the estimate below

| Problem in the estimate | What OpenCode shows |
|---|---|
| Tools must go through the host's pipeline | Solved, with MCP plus two hooks. The host needs no HTTP listener if it can use the SDK's in-process server. cclaw has no Node runtime, so it would need a stdio or loopback MCP server, as the estimate says. |
| Approval flow | No `--permission-prompt-tool` wiring is needed. The parked `tools/call` is the approval wait: the host asks the human and answers the call afterwards. `MCP_TOOL_TIMEOUT` must be unset. |
| History cannot be seeded | Still true. The workaround is one checkpoint user message, and a sync layer to decide what Claude has already seen. This is the expensive part. |
| One process per session | Not avoided. OpenCode accepts it, with a 5-minute idle stop and resume by session id. |
| Node and a logged-in `claude` on the box | Not avoided. The SDK install is about 270 MB. |

For cclaw the fit is close in one way and poor in two. `entries` as the source
of truth, tools run by the trusted parent, and one approval path are the same
choices OpenCode made. But cclaw builds each request from `entries` with a SQL
query and sends it on a worker thread, and this route needs a second
transcript, a sync layer and a child process per session. It also cannot run
on a host without Node.

## The Agent SDK underneath

Read 2026-11-13: the TypeScript SDK as published on npm
(`@anthropic-ai/claude-agent-sdk` 0.3.323, a minified bundle) and the Python
SDK source (`anthropics/claude-agent-sdk-python`, MIT licence), which is
readable and is the better reference for the protocol.

- **The SDK is a wrapper around the `claude` program.** It contains no model
  client. It starts `claude` as a child process and talks to it over the
  child's stdin and stdout. The TypeScript package gets the program as an
  optional per-platform dependency (linux x64 and arm64, glibc and musl,
  macOS, Windows; no 32-bit ARM). A host can point at another copy with
  `pathToClaudeCodeExecutable`, which is what OpenCode does.
- **It does not pass `-p`.** The fixed arguments are
  `--output-format stream-json --verbose --input-format stream-json`. `-p`
  is the one-shot form: one prompt in, the result out, exit. With
  `--input-format stream-json` the process stays up and both sides exchange
  JSON lines for the life of the session. Every option of the SDK becomes one
  more flag (`--tools`, `--allowedTools`, `--permission-mode`,
  `--setting-sources`, `--mcp-config`, `--strict-mcp-config`, `--resume`,
  `--session-id`, `--model`, `--append-system-prompt`, ...).
- **It says what it is.** The SDK sets `CLAUDE_CODE_ENTRYPOINT` to `sdk-ts` or
  `sdk-py` in the child's environment.

### The protocol on the pipes

One JSON object on each line, in both directions.

| Direction | `type` | Use |
|---|---|---|
| host → claude | `user` | A user message: `{"type":"user","session_id":"","message":{"role":"user","content":...},"parent_tool_use_id":null}` |
| claude → host | `system`, `assistant`, `user`, `stream_event`, `result` | The conversation as it happens. `result` ends a turn. |
| both | `control_request` | A request that needs an answer. It carries a `request_id` and a `request` with a `subtype`. |
| both | `control_response` | The answer: `{"type":"control_response","response":{"subtype":"success","request_id":...,"response":{...}}}`, or `subtype: "error"`. |

The control requests that this design needs:

- `initialize` (host → claude, first message). It registers the hooks: for
  each event, a matcher and a list of `hookCallbackIds` that the host chose.
- `hook_callback` (claude → host). Claude Code runs a registered hook. The
  request carries the `callback_id`, the hook `input` and the `tool_use_id`.
  The response is the hook's output.
- `mcp_message` (claude → host). This is how an in-process MCP server works.
  The host names a server in `--mcp-config` with `"type":"sdk"` and no command
  and no URL. Claude Code then sends each MCP JSON-RPC message for that server
  inside a control request (`server_name`, `message`), and the host returns
  the JSON-RPC answer as `mcp_response`. No socket and no second process.
- `can_use_tool` (claude → host), when the host asks for permission prompts
  over the pipe (`--permission-prompt-tool=stdio`). OpenCode does not use it.
- `interrupt` (host → claude) stops the running turn.

The SDK has about sixty more subtypes (model and permission-mode changes, MCP
status, session rename, file rewind, and user-interface messages). None is
needed here.

**Not known:** whether Anthropic treats this protocol as a stable interface
for programs other than its own SDKs. The Python source is public and MIT,
and the command-line flags are documented, but no document read for this
survey promises the control messages. The older note in the estimate below
("documented/stable") is too strong.

## How cclaw could copy the OpenCode design

Nothing here is built or decided. It is the shape the work would take, so that
the cost is known.

### What stays the same in cclaw

- `entries` is the source of truth for the session.
- The trusted parent runs every tool: grants, approvals, the namespace
  sandbox, the egress proxy, secret interpolation and the secret scanner all
  apply unchanged, because Claude never runs a tool itself.
- The mid-turn invariant holds: only model output and tool results enter
  `entries` during a turn.

### The pieces

1. **A third endpoint type** beside `openai` and `gemini`, for example
   `claude-agent`. A provider row of this type has no `base_url` and no key.
   It has a path to `claude`.
2. **The child.** One `claude` process for each active session, started with
   the fixed flags above and with the OpenCode isolation set: `--tools ""`,
   `--allowedTools "mcp__cclaw__*"`, `--permission-mode dontAsk`,
   `--setting-sources ""`, `--strict-mcp-config`, and the environment
   variables in OpenCode's `ISOLATION_ENV`. cclaw sends its system prompt
   with `--append-system-prompt`. Claude Code's own prompt stays in front.
3. **The pipe protocol in C.** Read and write JSON lines. SQLite JSON1 can
   build and parse them, as it does for the other wire formats. No Node and
   no SDK.
4. **An MCP server inside the daemon**, reached only through `mcp_message`.
   It needs four JSON-RPC methods: `initialize`, `notifications/initialized`,
   `tools/list` and `tools/call`. `tools/list` is a query over the `tools`
   table, which already holds each tool's JSON schema, filtered by the
   agent's grants as the other two builders do.
5. **Two hooks**, registered in `initialize`: `PreToolUse` to learn the
   `tool_use_id` of the call that is about to arrive, and `PostToolUse` only
   if a tool must return media.

### How one turn runs

| Step | Other providers | This provider |
|---|---|---|
| Iteration 1 | Build the request from `entries`, POST it, parse the response | Write one `user` line to the child, read lines until an `assistant` message is complete |
| A tool call arrives | It is in the response body | The child sends `hook_callback` (the id), then `mcp_message` with `tools/call` |
| cclaw runs the tool | `advance_session()` dispatches the batch | The same. The `tools/call` request stays unanswered while cclaw runs the tool, including an approval wait |
| Next iteration | Build a new request with the tool results, POST it | Answer the pending `tools/call` with the result, read lines until the next `assistant` message |
| End of turn | A response with no tool calls | A `result` line |

Each `assistant` message becomes entries the same way a response body does:
text, reasoning, and one `tool_call` entry for each call, with the id from
the hook.

### What does not fit, and the likely answer

- **A process for each session, alive between iterations.** The worker
  threads make one blocking request each and keep nothing. Here the child and
  its pipes must live from one iteration to the next. The event loop already
  owns forked tool children (`child.c`); it would own these too, and a
  session row would hold the child's Claude session id for `--resume`. A dead
  child is then recoverable: start a new one with `--resume`. A tool call that
  was in flight is lost, and Claude records it as cancelled.
- **cclaw rebuilds the visible history for every request; Claude Code keeps
  its own.** For the other providers the request body is a fresh query over
  `entries` each time, so anything that changes what the model should see
  takes effect on the next request. Claude Code holds a transcript and only
  accepts new messages at the end. cclaw changes the visible history in these
  ways, and each one makes Claude's transcript wrong:
  - **Model fallback.** The routing list can move a session to another model
    and back. Claude has not seen the entries in between.
  - **`session_condense`.** The agent replaces a range of its own past turns
    with a summary. This is an edit in the middle of the history.
  - **Compaction.** Old turns are replaced by a compaction entry.
  - **The read-time window.** When compaction fails, `plan_find_cut` drops old
    turns from the request without deleting them.
  - **Hook directives.** A hook can `suppress` an entry or inject text for one
    request only.
  - **The entry tree.** A session is a tree with a `leaf_id`; the visible
    history is the branch from the leaf to the root. Anything that moves the
    leaf to another branch replaces the history.

  OpenCode has the same problem for edit, undo and plugins that rewrite
  history. Its answer has two parts, and cclaw would need both:
  1. Keep a record of what Claude has seen (OpenCode fingerprints the
     history). Before each turn, compare it with the history cclaw would send.
  2. If cclaw's history only adds to what Claude has seen, send the new part.
     If it differs earlier than the end, start a fresh Claude session and
     send the whole visible history as one `<conversation-checkpoint>` user
     message. (OpenCode also copies Claude's session up to the last matching
     point, to keep the prompt cache. A first version can skip that and pay
     for the cache miss.)

  Turn Claude Code's own compaction off, so that cclaw's is the only one.

  Per-request hook directives do not fit this model: a `suppress` or an
  inject that is different on each request would restart the Claude session
  on each request. They would have to be unsupported on this provider, or
  limited to the turn boundary.
- **Parallel tool calls** are lost. Calls arrive one at a time.
- **Tool names and descriptions.** A name is at most 64 characters including
  `mcp__cclaw__` (12), so 52 are left; a description is cut at 2048
  characters. Extension tools must be checked against both.
- **Accounting.** There is no HTTP response to archive in `llm_responses`.
  The lines of the stream are the nearest thing. Usage needs OpenCode's rule
  (keep the highest count for each response), and cost is 0 on a subscription.
- **The retry and health policy** in `llm_req()` does not apply. Claude Code
  retries by itself. A plan limit is a terminal error for the model, and the
  routing list can then fall back as it does for any other failed model.
- **Containment of the child.** `claude` is a large program that is not ours.
  With its tools off it still reads `~/.claude` and talks to Anthropic. It
  could run inside the namespace sandbox with only its credentials mounted
  and egress limited to Anthropic's hosts. Not examined.
- **What the child should say it is.** The SDKs set `CLAUDE_CODE_ENTRYPOINT`.
  cclaw would not be an SDK. Do not copy an SDK's value to look like one; the
  survey above shows where that road goes. What a non-SDK host should send is
  an open question for Anthropic's documents.
- **Hosts.** `claude` has no build for ARMv5 or 32-bit x86. This provider is
  for the 64-bit targets only, and it needs a signed-in `claude` on the box
  (`claude setup-token` for a box with no browser).

### Smallest experiment

On the dev box, by hand, before any C:

1. Start `claude` with the flags above and one `sdk` MCP server in
   `--mcp-config`. Drive the pipes from a script.
2. Send `initialize` with a `PreToolUse` hook, then one `user` line that makes
   the model call the tool.
3. Check that the `hook_callback` and the `mcp_message` arrive, hold the
   `tools/call` for a minute, answer it, and read the final `result`.
4. Record: time to first token, whether a held call survives the wait, what
   `/usage` shows afterwards, and whether the run is charged to the plan.

If step 3 does not work without an SDK in the middle, the design needs Node
and the estimate changes.

## Cost of the sanctioned route in cclaw (`claude -p` subprocess)

Recorded here for when this is revisited:

- New `endpoint_type`: worker thread writes a `stream-json` line to a child's
  stdin, reads streamed JSON lines back, instead of building/parsing an HTTP
  body. The wire protocol is plain JSON lines and is visible in the MIT
  Python SDK, so cclaw could speak it directly without any Node/Python SDK
  layer. Whether it is a stable interface is not known (see "The Agent SDK
  underneath").
- **Process model conflict**: this needs a persistent-or-resumed subprocess per
  session (`--resume <id>`), which cuts against "long-lived core, disposable
  work" and introduces exactly the kind of parallel state (child pid ↔
  session) `AGENTS.md` warns about. Spawning fresh per turn with `--resume`
  avoids a long-lived child but still means a subprocess per turn on this one
  provider, unlike every other provider.
- **Tool execution**: Claude Code runs its own tools against the real
  filesystem by default. To keep cclaw's sandbox/approval/secret-scanner
  pipeline in the loop, tools would need to be exposed to the child as an MCP
  server (loopback HTTP via civetweb) with the child restricted to
  `mcp__cclaw__*` — a real new subsystem (MCP handshake, schema export,
  capture-token auth), plus wiring the `--permission-prompt-tool stdio`
  control-request channel into cclaw's approval flow.
- **Session/context ownership**: Claude Code keeps its own transcript and
  compaction. cclaw's `entries`/turn model would either mirror it lossily or
  cede context ownership for sessions on this provider — direct tension with
  "state has one home." There is no supported way to seed a Claude Code
  session with arbitrary prior messages (resume/fork/continue only), so a
  cclaw session's existing history can't be transplanted in.
- **Deployment mismatch**: requires Node + a logged-in Claude Code install —
  fine on the EC2 dev/prod box, a non-starter on Pogoplug-class targets.

## Decision (2026-08-26)

**No OAuth/subscription integration for Claude in cclaw for now.** Reasons:

1. The only mechanism that reaches your plan's *included* usage (not the
   overage lane) is the `claude` CLI subprocess, and it requires real
   architectural compromises (subprocess-per-session, MCP tool bridge,
   split session/context ownership) that conflict with several load-bearing
   cclaw invariants.
2. The raw-HTTP/impersonation route is an active arms race on Anthropic's
   side (see Hermes above) — the kind of compat-shim treadmill AGENTS.md
   explicitly says not to build, and it doesn't even reach the plan's
   included limits.
3. Anthropic's own billing policy for this has changed three times in ten
   weeks (ban → credit plan → paused) as of this writing. Building against
   it now means building against a moving target that may make any of the
   above moot (or much easier) within months.

**Revisit when**: Anthropic ships a stable Agent SDK / `claude -p` billing
policy (the paused credit plan or its successor), *and/or* documents a
supported way to use subscription auth outside the CLI process. Until then,
Claude models are reached through OpenRouter (`specs/providers.md`) — the
native Anthropic Messages API builder is separately scoped out for unrelated
wire-format reasons (see `specs/providers.md`'s Anthropic row).

**Review 2026-11-13: decision unchanged.** OpenCode's provider shows that the
sanctioned route can keep the host's tools, approvals and history, so reason 1
is now a known cost and not an open question. The cost is a child process per
session, an MCP bridge, and a history sync layer, on a host with Node.
Anthropic's billing for this use is still at its discretion, so the "revisit
when" condition above is not met.

If this is taken up, the smallest useful experiment is on the dev box: start
`claude` with built-in tools off and one MCP tool, park the call, answer it
from outside, and measure latency and what the subscription is charged.

If a subscription route is revisited for OpenAI models specifically (not
Anthropic), the Codex raw-HTTP route is comparatively low-cost and
non-adversarial — but confirm native tool-calling support on
`chatgpt.com/backend-api/codex/responses` empirically before scoping any
implementation work, since cclaw's turn model assumes native tool calls.
