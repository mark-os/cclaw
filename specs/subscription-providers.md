# Subscription-Backed Providers (Claude / ChatGPT) — Survey & Decision

**Status as of 2026-08-26. This is a snapshot, not a roadmap — see Decision below.**

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

## Cost of the sanctioned route in cclaw (`claude -p` subprocess)

Recorded here for when this is revisited — see the fuller writeup in this
conversation's history:

- New `endpoint_type`: worker thread writes a `stream-json` line to a child's
  stdin, reads streamed JSON lines back, instead of building/parsing an HTTP
  body. Wire protocol is documented/stable, not SDK-exclusive — cclaw could
  speak it directly without any Node/Python SDK layer.
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

If a subscription route is revisited for OpenAI models specifically (not
Anthropic), the Codex raw-HTTP route is comparatively low-cost and
non-adversarial — but confirm native tool-calling support on
`chatgpt.com/backend-api/codex/responses` empirically before scoping any
implementation work, since cclaw's turn model assumes native tool calls.
