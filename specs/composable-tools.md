# Composable MJS Tools — Design

## Problem

CClaw MJS tools currently can't invoke shell commands or other tools from their
handler. The only IO available is `http_fetch` and `fs.*`. This means an MJS
tool can't wrap a CLI binary (like `gh`) into a typed, policy-gated interface.

The goal: MJS tools become a **composable layer above raw shell_exec** — the
agent calls a typed tool with structured arguments, a policy layer decides
allow/deny before execution, and the handler shells out as an implementation
detail.

## Motivation: GitHub as canonical example

Today, to use `gh` CLI the agent calls `shell_exec` with a raw command string:
```
shell_exec({"command": "gh pr merge 123 --auto"})
```

The only gating is trust_level (sandbox yes/no) and allowed_hosts (network).
There's no way to say "allow pr list but deny pr merge" without regexing the
command string at a hook level.

With a composable MJS tool:
```
github({"action": "pr_merge", "number": 123, "auto": true})
```

Policy can inspect structured args *before* the handler runs. The handler
constructs and executes the shell command internally.

## Current State

```
LLM calls tool
  → main process dispatches
  → js_tool_register_ext forks a child
  → child evaluates handler string with args as JSON
  → handler can use: http_fetch, fs.*, console.log
  → handler CANNOT use: shell_exec, callTool
  → child returns result string, exits
```

The MJS child runs in the same forked-process model as shell_exec itself, but
it has no mechanism to invoke a shell command from within its execution context.

## Design

### New primitive: `cclaw.exec(command, opts)`

Add a `cclaw.exec` global to the MJS tool handler environment. This is the
bridge from JS to the sandbox shell.

```javascript
// Available inside MJS tool handlers only (not js_eval, not channels)
var result = cclaw.exec("gh pr list --json number,title", {
    timeout: 30    // optional, seconds
});
// result = { stdout: "...", stderr: "...", exit_code: 0 }
```

**Implementation**: the MJS child (already forked) fork+execs `/bin/sh -c`
with the same namespace sandbox applied to normal shell_exec calls. The MJS
child's trust_level determines the sandbox profile. The call is synchronous
(blocks the JS handler until the shell command completes or times out).

This is NOT re-entrant into the agent — it's a direct fork+exec from the
already-forked MJS child. No IPC back to the parent. The MJS child is already
sandboxed at the same trust level, so the nested shell inherits that sandbox.

### New primitive: `cclaw.callTool(name, args)`

For tools that want to compose other *registered* tools (JS or C), add a
synchronous `cclaw.callTool` that dispatches back to the tool registry in the
parent process via a pipe/UDS protocol.

```javascript
var result = cclaw.callTool("web_fetch", { url: "https://api.github.com/..." });
```

**Implementation**: the forked MJS child sends a request over a pre-arranged
fd (pipe or socketpair) to the parent, which dispatches the tool call in-process
and writes the result back. Depth-limited (max 4) to prevent infinite recursion.

This is more complex than `cclaw.exec` and may be deferred to a later phase.
`cclaw.exec` alone covers the GitHub use case.

### Config & Authority Boundary

**An MJS tool never mutates capability/trust config.** It cannot grant itself (or
its agent) a tool, a host, a path, or a trust level. This is not a convention to
remember — it is structurally impossible, enforced by two independent walls:

1. **No DB handle in the sandboxed child.** The handler runs in a forked child
   that holds no SQLite handle. The static-net architecture keeps the network
   allowlist parent-side in the proxy and reaches the child only over a constrained
   text preamble; `src/main.c` wipes the inherited key in the child and comments
   *"This broker holds no DB handle"*. So `cclaw.exec` and the handler physically
   cannot reach config rows.
2. **Self-granting is the one thing the security model exists to prevent.** Per
   `specs/security.md` (intersection-not-union, V54): *a restricted agent cannot
   grant itself more permissions.* Capability changes go through human approval, by
   construction.

**The flow when a tool needs more authority.** A handler that hits a missing
capability (e.g. a host not in the allowlist) does **not** attempt any mutation. It
returns an **error string telling the agent to call `request_config`**:

```javascript
var r = cclaw.exec("gh ...");
if (/blocked|DENIED|ENETUNREACH/.test(r.stderr))
    return "error: host not allowed. Ask for access: "
         + "request_config({action:'grant_host', host:'api.github.com'})";
```

The **agent** then calls `request_config`; a human approves; the capability lands
on the **agent**, not the tool. `request_config` is the sole creator of
side-effecting (`resolve="apply"`) approvals, and its mutation logic lives in the
trusted parent (`resolve_approval()`) — a place an MJS handler can never author
code into. See "Reconciling with the approval model" below.

### Policy layer: `extensions.policy`

Policy is a **per-argument pre-filter, evaluated in C in the parent before the
fork**, sitting *in front of* the existing per-tool mode gate (the approval gate
every tool already passes through). It is not a parallel approval system — it
decides whether a call is even eligible to reach that gate.

Policy is a property of the **tool**, not the extension package (an extension is
an install/trust unit that *provides* 0..N tools). It is keyed on the tool name and
carried on the tool definition. Durable home is a `policy` column on the `tools`
table; the in-memory home is `ToolEntry.policy_json`, read at dispatch:

```sql
ALTER TABLE tools ADD COLUMN policy TEXT; -- JSON policy rules (authored, per-tool)
```

JS-defined tools source policy from their `registerTool({policy})` field (harvested
into the registry at load); builtin/promoted tools source it from `tools.policy`.
Either way it lands on `ToolEntry.policy_json` — no per-dispatch DB query.

Policy schema:
```json
{
  "rules": [
    {"match": {"action": "pr_merge"}, "effect": "ask"},
    {"match": {"action": "pr_list"}, "effect": "allow"},
    {"match": {"action": "issue_create", "labels": ["production"]}, "effect": "deny"},
    {"match": {}, "effect": "allow"}
  ]
}
```

Evaluation: first matching rule wins. If no rule matches, default is **allow**
(the tool is already in the agent's allowed set — policy refines, not gates).

**Match semantics**: each key in `match` is checked against the tool call's
arguments. Values can be:
- String literal: exact match
- Array: argument value must be one of the listed values
- `"*"`: any value (explicit wildcard, useful for documentation)
- Absent key: not checked

**Effects — mapped onto existing machinery, no new mechanism:**
- `allow` — fall through to the normal per-tool mode gate in `fork_tool_exec`.
- `deny` — return an error to the LLM, no fork.
- `ask` — set this call's gate to ASK, which takes the existing
  `approval_create(..., "rerun")` + park path: the frozen tool_call re-runs on
  approval, and the **human picks the mode** (once / always / deny) at approval
  time. This is not new work — it reuses the shipped `rerun` strategy.

### Reconciling with the approval model

The approval system carries a `resolve` strategy per approval:

- `resolve="rerun"` — the frozen tool_call re-dispatches and runs on approval.
- `resolve="apply"` — a side effect applied in the trusted parent
  (`resolve_approval()`), reserved for built-in C tools whose mutation lives there.

**MJS tools only ever create `resolve="rerun"` approvals** (via the `ask` effect or
their standing tool mode). They never create `resolve="apply"` — that path exists
only for `request_config`'s grants, which an MJS handler cannot reach (no DB handle,
no parent-side code). The agent has no runtime path to set its own tool modes or
policy; the human chooses the mode at approval time.

### Where policy is stored

**One level: the authored per-tool policy** (`tools.policy` / `ToolEntry.policy_json`,
never `extensions.policy`). Policy is set by the tool
author at registration and is **not agent-mutable at runtime** — there is no
per-agent override and no self-service tighten/loosen. If a runtime policy change
is ever genuinely needed, it is a human-approved `request_config` `apply` action,
not something the agent or its tools can do on their own.

### Tool registration with policy (JS API)

> **Convention:** pass `parameters` and `policy` as **plain objects**, not
> `JSON.stringify(...)` strings — the harvester (`process_registered_tools`)
> stringifies them, so pre-stringifying double-encodes. (Some examples below still
> show `JSON.stringify`; treat the object form as canonical.)

```javascript
cclaw.registerTool({
    name: "github",
    description: "GitHub CLI operations",
    parameters: JSON.stringify({
        type: "object",
        properties: {
            action: {
                type: "string",
                enum: ["pr_list", "pr_create", "pr_merge",
                       "issue_list", "issue_create", "issue_comment",
                       "repo_clone"]
            },
            // ... action-specific params
        },
        required: ["action"]
    }),
    policy: JSON.stringify({
        rules: [
            {match: {action: "pr_merge"}, effect: "deny"},
            {match: {action: "repo_clone"}, effect: "allow"},
            {match: {}, effect: "allow"}
        ]
    }),
    handler: "var cmd = build_gh_command(args);\n"
           + "var result = cclaw.exec(cmd, {timeout: 30});\n"
           + "return result.stdout || result.stderr;"
});
```

### Dispatch flow with policy

The policy pre-filter runs first; `allow`/`ask` then fall through to the existing
per-tool mode gate in `fork_tool_exec`. Policy never replaces the gate, it feeds it.

```
LLM calls github({"action": "pr_merge", "number": 123})
  → main process: lookup tool "github" in registry
  → main process: read the tool's policy (ToolEntry.policy_json), evaluate against args
  → effect = deny  → return "error: action 'pr_merge' denied by policy" (no fork)
  → effect = ask   → set this call's gate to ASK → existing approval_create(...,"rerun")
                     → park; human picks once/always/deny → frozen call re-runs on approve
  → effect = allow → fall through to the normal per-tool mode gate (fork_tool_exec)
                     → fork MJS child, execute handler
                       → handler calls cclaw.exec("gh pr list --json ...")
                       → nested fork+exec /bin/sh inside same sandbox
                       → result flows back to LLM
```

Policy evaluation happens in C in the main process before forking. Zero cost for
denied calls. `ask` and `allow` both proceed to the same mode gate every tool
already passes — policy only decides eligibility, the gate decides approval.

## The GitHub Tool in MJS

```javascript
// workspace/extensions/github.js
(function(cclaw) {
    cclaw.registerTool({
        name: "github",
        description: "Interact with GitHub via the gh CLI. "
            + "Actions: pr_list, pr_create, pr_merge, pr_view, "
            + "issue_list, issue_create, issue_comment, repo_clone, repo_view",
        parameters: JSON.stringify({
            type: "object",
            properties: {
                action: {
                    type: "string",
                    enum: ["pr_list", "pr_create", "pr_merge", "pr_view",
                           "issue_list", "issue_create", "issue_comment",
                           "repo_clone", "repo_view"],
                    description: "The GitHub operation to perform"
                },
                repo: {
                    type: "string",
                    description: "owner/repo (uses current repo if omitted)"
                },
                number: {
                    type: "integer",
                    description: "PR or issue number (for view/merge/comment)"
                },
                title: { type: "string" },
                body: { type: "string" },
                base: { type: "string", description: "Base branch for PR" },
                labels: {
                    type: "array", items: { type: "string" }
                },
                auto_merge: { type: "boolean" }
            },
            required: ["action"]
        }),
        policy: JSON.stringify({
            rules: [
                {match: {action: "pr_merge"}, effect: "deny"},
                {match: {action: "pr_create"}, effect: "allow"},
                {match: {action: "issue_create"}, effect: "allow"},
                {match: {}, effect: "allow"}
            ]
        }),
        handler: [
            "var a = args.action;",
            "var repo = args.repo ? ' -R ' + args.repo : '';",
            "var cmd = 'gh';",
            "",
            "if (a === 'pr_list') {",
            "  cmd = 'gh pr list --json number,title,state,url' + repo;",
            "} else if (a === 'pr_view') {",
            "  cmd = 'gh pr view ' + args.number + ' --json title,body,state,url' + repo;",
            "} else if (a === 'pr_create') {",
            "  cmd = 'gh pr create --title ' + JSON.stringify(args.title || '')",
            "    + ' --body ' + JSON.stringify(args.body || '')",
            "    + (args.base ? ' --base ' + args.base : '') + repo;",
            "} else if (a === 'pr_merge') {",
            "  cmd = 'gh pr merge ' + args.number",
            "    + (args.auto_merge ? ' --auto' : ' --merge') + repo;",
            "} else if (a === 'issue_list') {",
            "  cmd = 'gh issue list --json number,title,state,url' + repo;",
            "} else if (a === 'issue_create') {",
            "  cmd = 'gh issue create --title ' + JSON.stringify(args.title || '')",
            "    + ' --body ' + JSON.stringify(args.body || '')",
            "    + (args.labels ? ' --label ' + args.labels.join(',') : '') + repo;",
            "} else if (a === 'issue_comment') {",
            "  cmd = 'gh issue comment ' + args.number",
            "    + ' --body ' + JSON.stringify(args.body || '') + repo;",
            "} else if (a === 'repo_clone') {",
            "  cmd = 'gh repo clone ' + (args.repo || '') + ' .';",
            "} else if (a === 'repo_view') {",
            "  cmd = 'gh repo view --json name,description,url' + repo;",
            "} else {",
            "  return 'error: unknown action ' + a;",
            "}",
            "",
            "var r = cclaw.exec(cmd, {timeout: 30});",
            "if (r.exit_code !== 0) return 'error (exit ' + r.exit_code + '): ' + r.stderr;",
            "return r.stdout;"
        ].join("\n")
    });
})(cclaw);
```

## Implementation Phases

### Phase 1: `cclaw.exec` (minimal, unblocks composable tools)

- Add `cclaw_exec` native function to the MJS tool child environment
- Implementation: `fork()` + `exec("/bin/sh", "-c", cmd)` from the MJS child
- Apply same sandbox as shell_exec (inherit trust_level from agent)
- Timeout via alarm/waitpid
- Return `{stdout, stderr, exit_code}` as JS object
- Scope: only available in tool handler context (not js_eval, not channels)

### Phase 2: Policy layer

- Add the authored `policy` column to the `tools` table (per-tool, not on
  `extensions`); carry it on `ToolEntry.policy_json`, sourced from
  `registerTool({policy})` for JS tools and `tools.policy` for builtin/promoted
- Policy evaluation in C (jsmn parse of policy JSON, match args), before the fork,
  reading `ToolEntry.policy_json` (no per-dispatch DB query)
- `deny` → error to LLM (no fork); `allow`/`ask` → fall through to the existing
  per-tool mode gate, with `ask` taking the shipped `approval_create(..., "rerun")`
  + park path (human picks once/always/deny)
- Wire MJS tool dispatch into `tool_needs_interpolation()` (or a `tools`-table flag)
  so `{{SECRET:name}}` resolves in `cclaw.exec` args

### Phase 3: `cclaw.callTool` (optional, deferred)

- Socketpair between parent and MJS child
- Request/response protocol: `{tool, args}` → `{result}`
- Depth limit (4) enforced by parent
- Enables MJS tools calling other MJS tools or C tools (web_fetch, file_read)
- Adds complexity; only implement if real use cases demand it

## Tool-scoped data

Tools that need to persist non-authority operational data (cursors, caches, a last-
synced timestamp, per-tool preferences) write it via `fs.*` under a conventional
workspace path, e.g. `workspace/.tooldata/<tool>.json`. This is the release valve
that keeps capability config from being abused as a scratchpad.

It is explicitly **not** the config tables (`extensions`, `agents`, `grants`, …)
and **not** the approval gate. It carries no authority: it cannot grant a tool, a
host, a path, or a trust level. It is just files in the agent's own workspace.

## Security Considerations

- `cclaw.exec` inherits the agent's trust_level sandbox — no privilege escalation
- Policy evaluation is in C, before JS runs — can't be bypassed from handler
- **The forked child has no DB handle** — config rows are unreachable from a
  handler or `cclaw.exec`, so an MJS tool cannot mutate capability/trust config
  (see "Config & Authority Boundary"). It can only create `resolve="rerun"`
  approvals, never `resolve="apply"`.
- **Secret interpolation requires wiring `cclaw.exec` into the interpolation set.**
  `{{SECRET:name}}` resolution is gated by a per-tool allowlist in `src/main.c`
  (currently `shell_exec`, `web_fetch`, `js_eval`). A forked MJS tool's args are
  **not** interpolated unless MJS tool dispatch is added to that allowlist — or the
  property becomes a flag on the `tools` table. This is a required implementation
  step for the GitHub-via-`gh` example to work, not automatic.
- Handler code is workspace-scoped (agent authored) — same trust as a draft extension
- Network from cclaw.exec goes through the same proxy/allowed_hosts gate
- **`cclaw.callTool` (if built) must denylist config/authority tools** —
  `request_config` and any tool promotion / define path. A synchronous, blocking,
  forked handler cannot suspend-and-resume across a human approval, so any tool that
  *parks* (every `apply` approval does) is fundamentally un-composable via
  `callTool`. The OpenClaw Lobster reference reinforces this: approval gates use
  durable resume tokens and are *disabled in sandboxed contexts*, never blocking
  nested calls.

## Comparison to OpenClaw

| Concern | OpenClaw | CClaw (this design) |
|---------|----------|---------------------|
| Typed tool surface | Plugins register tools via SDK | MJS registerTool with JSON schema |
| Shell access | exec tool (binary) | cclaw.exec inside handler (hidden from LLM) |
| Policy | exec approvals (binary+argPattern regex) | Structured JSON rules on typed args |
| Approval UX | allow-once / allow-always interactive | per-rule deny/allow; `ask` reuses the `rerun` approval path (human picks once/always/deny) |
| Composability | Lobster pipelines for multi-step | cclaw.callTool for tool-calls-tool |
| Gating granularity | Per-binary + arg regex | Per-argument-value on structured schema |

The CClaw model is simpler (no regex parsing of command strings) and more
precise (policy operates on typed JSON, not shell tokenization).

## Candidate MJS Tools (from OpenClaw skills catalog)

### Pure HTTP API — no CLI needed (best candidates for MJS tools)

| Tool | API surface | Policy value |
|------|-------------|--------------|
| **github** | REST API (pulls, issues, runs, releases) | Gate pr_merge, issue_close; allow reads freely |
| trello | REST API (boards, cards, lists) | Gate card_move, card_delete |
| notion | REST API (pages, databases, blocks) | Gate page_delete, database_update |
| slack | REST API (messages, channels, users) | Gate message_send by channel |
| discord | REST API (messages, channels, guilds) | Gate message_send, kick, ban |
| linear | REST/GraphQL (issues, projects, cycles) | Gate issue_close, state changes |

### CLI-bound — need `cclaw.exec` (local process / hardware / native app)

| Tool | Why CLI-only | Policy value |
|------|--------------|--------------|
| **tmux** | Local unix socket to terminal multiplexer | Gate kill-session, new-window |
| obsidian | IPC to running desktop app | Gate delete, property changes |
| spotify/spogo | Local daemon + cookie auth | Gate play (harmless) vs playlist_delete |
| apple-notes/reminders | AppleScript / macOS Shortcuts | Gate delete, create |
| bear-notes | macOS URL schemes + AppleScript | Gate trash, archive |
| things-mac | AppleScript | Gate complete, delete |
| imsg (iMessage) | AppleScript / BlueBubbles local | Gate send by recipient |
| openhue | Local mDNS bridge (HTTP to bridge, but discovery is local) | Gate scene changes |
| blucli | Bluetooth system commands | Gate connect/disconnect |
| camsnap | Local camera hardware | Gate capture (privacy) |

### Hybrid — API exists but CLI is more practical

| Tool | Notes |
|------|-------|
| himalaya (email) | IMAP/SMTP protocol directly is possible but painful; CLI handles auth/TLS |
| gh-issues | Subset of github, same REST API works |

## Recommended First MJS Tool: `github`

**Why github:**
1. Clean REST API — no CLI dependency, works everywhere
2. Clear action taxonomy — reads vs writes vs destructive ops
3. Obvious policy value — allow reads, gate merges/closes
4. Most developers already have a GitHub token
5. Tests the full design: schema, policy, http_fetch, secret interpolation

**What it proves:**
- MJS tool with typed `action` enum works
- Policy layer catches destructive calls before execution
- `http_fetch` (extended with method/headers/body) is sufficient for real work
- No `cclaw.exec` needed — validates the pure-API path first

**Second tool: tmux or himalaya**
- tmux proves the `cclaw.exec` path (local process, no API)
- himalaya proves a hybrid (could be API but CLI is practical)

Build github first (Phase 1 only needs `http_fetch` enhancement), then tmux
to validate `cclaw.exec`.
