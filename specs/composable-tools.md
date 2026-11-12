# Composable JS Tools — Design

Status:

| Part | State |
|------|-------|
| Static policy floor (`tools.policy`, `policy_eval`) | implemented |
| JS policy ratchet (`policy(args)` in the handler file) | not implemented |
| `cclaw.exec` | not implemented |
| `cclaw.callTool` | not implemented |
| Content-hash pre-fork check | not implemented |

Tools are declared in an extension manifest and implemented in `.qjs` handler
files; [extensions.md](extensions.md) is the contract for both. This file
covers the policy layer and the planned composition primitives.

## Problem

A JS tool handler cannot run shell commands or call other tools. Its only IO
is `http_request` and `fs.*`. So a JS tool cannot wrap a CLI binary (like `gh`)
into a typed, policy-gated interface.

The goal: JS tools become a **composable layer above raw shell_exec** — the
agent calls a typed tool with structured arguments, a policy layer decides
allow/deny before execution, and the handler shells out as an implementation
detail.

## Motivation: GitHub as canonical example

Today, to use `gh` CLI the agent calls `shell_exec` with a raw command string:
```
shell_exec({"command": "gh pr merge 123 --auto"})
```

The only gating is the sandbox profile and the agent's host grants.
There's no way to say "allow pr list but deny pr merge" without regexing the
command string at a hook level.

With a composable JS tool:
```
github({"action": "pr_merge", "number": 123, "auto": true})
```

Policy can inspect structured args *before* the handler runs. The handler
constructs and executes the shell command internally.

## Current State

```
LLM calls tool
  → main process dispatches (SBX_JS tier)
  → secret interpolation in parent (pre-serialization)
  → serialize RunToolReq blob → fork+execve cclaw --run-tool
  → broker: run_tool_verify_clean, parse blob, tier_descriptor(RUNTOOL_TIER_JS)
  → serve_network_child: proxy_bind, fork inner child
    → inner child: namespace sandbox, tool_js_tier_run(q)
      → tool_js_eval_handler: QuickJS eval of handler file
      → handler can use: http_request (curl via HTTP_PROXY → proxy), fs.*
      → handler CANNOT use: shell_exec, callTool
    → inner child writes result to pipe, _exit
  → broker drains output, reaps child, writes result back
  → daemon collects result, stores in DB
```

JS tools (both `js_eval` and JS-defined extension tools) run through the same
`--run-tool` broker path as shell and web. The dispatch in `src/dispatch.c`
routes via `te->recipe.tier == SBX_JS`; `tool_js_tier_run` (`src/tool_js.c`) is
the in-process leaf in the inner fork.

## Design

### New primitive: `cclaw.exec(command, opts)` (not yet implemented)

Add a `cclaw.exec` global to the JS tool handler environment. This is the
bridge from JS to the sandbox shell.

```javascript
// Available inside JS tool handlers only (not js_eval, not channels)
var result = cclaw.exec("gh pr list --json number,title", {
    timeout: 30    // optional, seconds
});
// result = { stdout: "...", stderr: "...", exit_code: 0 }
```

**Implementation**: the JS child (already forked) fork+execs `/bin/sh -c`
with the same namespace sandbox applied to normal shell_exec calls. The JS
child's sandbox_profile determines the sandbox profile. The call is synchronous
(blocks the JS handler until the shell command completes or times out).

This is NOT re-entrant into the agent — it's a direct fork+exec from the
already-forked JS child. No IPC back to the parent. The JS child is already
sandboxed at the same trust level, so the nested shell inherits that sandbox.

### New primitive: `cclaw.callTool(name, args)` (not yet implemented)

For tools that want to compose other *registered* tools (JS or C), add a
synchronous `cclaw.callTool` that dispatches back to the tool registry in the
parent process via a pipe/UDS protocol.

```javascript
var result = cclaw.callTool("web_fetch", { url: "https://api.github.com/..." });
```

**Implementation**: the forked JS child sends a request over a pre-arranged
fd (pipe or socketpair) to the parent, which dispatches the tool call in-process
and writes the result back. Depth-limited (max 4) to prevent infinite recursion.

This is more complex than `cclaw.exec` and may be deferred to a later phase.
`cclaw.exec` alone covers the GitHub use case.

### Config & Authority Boundary

**A JS tool never mutates capability/trust config.** It cannot grant itself (or
its agent) a tool, a host, a path, or a trust level. This is not a convention to
remember — it is structurally impossible, enforced by two independent walls:

1. **No DB handle in the sandboxed child.** The handler runs in a forked child
   that holds no SQLite handle. The static-net architecture keeps the network
   allowlist parent-side in the proxy and reaches the child only over a constrained
   text preamble; the `--run-tool` entry is intercepted at the top of `main()`
   before any DB, config or key initialization, so the broker holds no DB handle. So `cclaw.exec` and the handler physically
   cannot reach config rows.
2. **Self-granting is the one thing the security model exists to prevent.** Per
   `specs/security.md` (intersection-not-union): *a restricted agent cannot
   grant itself more permissions.* Capability changes go through human approval, by
   construction.

**The flow when a tool needs more authority.** A handler that hits a missing
capability (e.g. a host not in the allowlist) does **not** attempt any mutation. It
returns an **error string telling the agent to call `request_config`**:

```javascript
var r = cclaw.exec("gh ...");
if (/blocked|DENIED|ENETUNREACH/.test(r.stderr))
    return "error: host not allowed. Ask for access: "
         + "request_config({changes:"
         + "{grants:{hosts:['api.github.com']}}})";
```

The **agent** then calls `request_config`; a human approves; the capability lands
on the **agent**, not the tool. `request_config` is the sole creator of
side-effecting (`resolve="apply"`) approvals, and its mutation logic lives in the
trusted parent (`resolve_approval()`) — a place a JS handler can never author
code into. See "Reconciling with the approval model" below.

### Definition is data: install → load → call

**The DB is the source of truth for tool definitions; the `.qjs` file is only the
implementation, referenced by `path`.** This is the AGENTS.md contract ("the DB
stores the definition and a path; it never stores code").

Every tool — builtin or JS — is **one row in `tools`**
(`name, description, parameters_json, path, policy, agent_name, enabled`).
`llm_payload.c` builds the request's tool list from this table, filtered by the
agent's tool grants. Only the handler resolution differs:

| | Definition | Handler |
|---|---|---|
| **Builtin** | `tools` row, `extension_name=NULL`, `path=NULL` (synced from C by `tools_sync_to_db`) | C function, looked up by name |
| **JS tool** | `tools` row, `extension_name=<ext>`, `path=<file>` | the `.qjs` file at `path` |

- **Install / promote** (rare, audited — the trust boundary).
  `extension_promote` validates `extension.json` (`src/extension_manifest.c`):
  each tool's `parameters` schema, its `policy` shape, its `hosts`, and that
  each handler is a bundle-relative `.qjs` file. On success it copies the
  bundle to the shared store and writes the `extensions`, `tools` and
  `agent_extensions` rows. It never evaluates the handler.
- **Load** (every startup, pure DB read, no JS evaluation).
  `tools_load_extension_tools` (`src/tools.c`) joins `tools` →
  `agent_extensions` → `extensions` and records schema, policy and `path` on
  each `ToolEntry`.
- **Call** (per invocation). The parent forks `--run-tool` with the
  `RUNTOOL_TIER_JS` request. The inner child wraps the handler file as a
  function body with `args` in scope and calls it (see
  [extensions.md](extensions.md#tool-handler-contract)).

> **Content-hash pre-fork check (not implemented).** The design stores a
> content hash of the handler file on `extensions` at install and verifies it
> before each fork, failing closed on mismatch. There is no `extensions.hash`
> column. The shared store's read-only mount covers most of the same risk.

### Policy: authored on the tool, evaluated pre-fork

Policy is a **per-argument pre-filter, evaluated in the parent before the fork**,
sitting *in front of* the existing per-tool mode gate (the approval gate every tool
already passes through). It is not a parallel approval system — it decides whether a
call is even eligible to reach that gate.

Policy is a property of the **tool**, not the extension package (an extension is an
install/trust unit that *provides* 0..N tools). Its durable home is the `policy`
column on the `tools` table (already present); the in-memory home is
`ToolEntry.policy_json`, populated at load from the row — **no
per-dispatch DB query**.

Effects are ordered `allow < ask < deny`. A tool's policy is **two layers, and the
effective effect for a call is the `max` of them**:

1. **Static floor** (`tools.policy`, JSON rules, C-evaluated, zero-cost). Sets the
   per-action *default* — "reads → allow, `send` → ask," etc. This is the form most
   tools need, and on its own it is the whole policy.
2. **JS ratchet** (*not implemented*; an optional `policy(args) → "allow"|"ask"|"deny"` authored in the
   `.qjs`). Its return is **`max`'d with the floor**, so it can only *raise*
   restriction — deny, ask, or `allow` (a no-op that leaves the floor standing). It
   can **never** make a call more permissive than the floor. Use it for decisions the
   exact-match grammar can't express (the `email` recipient/domain case below).

This is not two competing forms — it is one model where data carries the part you
must audit and JS carries the part that's safe not to:

- **The static floor is the audited ceiling: maximum permissiveness, in plain data.**
  A human reading `send → allow` vs `send → ask` at install knows the worst case in
  one line. That single value *is* the fail-open surface.
- **The JS ratchet needs no permissiveness audit, because `max(floor, js)` makes
  over-permission structurally impossible.** Its audit collapses to "is it pure,
  args-only, bounded?" — not "does it ever wrongly allow?" It can't. So a
  `transfer → ask` floor means no JS return, however buggy or compromised, can ever
  produce a silent transfer.

**Where the JS runs.** Same machinery as the existing gating hooks
(`hook_dispatch_gate_tool_call`): in the parent, **pre-fork**, so a `deny` costs no
process and an `ask` lands directly in the parent's approval path (no IPC). The
`manifest.json` declares its extent — pure function of `args`, no network/fs — and
the install audit holds it to that envelope; the parent caps it with an
instruction/time budget (the bound the gating hooks already need) so it can't hang
the daemon.

Above the tool, the per-agent grant restriction and gating hooks compose the same
way (`max`, restrict-only) — every layer may add friction, none may remove it.

Static-floor schema:
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

**Match semantics** (`policy_eval`, `src/tool_policy.c`, SQLite JSON1): each
key in `match` is checked against the tool call's arguments. Values can be:
- String literal: the argument is text and equals it
- Array: the argument is text and equals one of the listed strings
- `"*"`: the argument is present, any type
- A key not named in `match` is not checked. A key named in `match` but absent
  from the arguments fails the rule.

A malformed policy (not an object, `rules` not an array, a rule without a
`match` object, an unknown effect) is `POLICY_ERROR`: the call is blocked with
a model-visible error. It never falls open to allow.

**Effects — mapped onto existing machinery, no new mechanism:**
- `allow` — fall through to the normal per-tool mode gate in `dispatch_tool_inner` (`src/dispatch.c`).
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

**JS tools only ever create `resolve="rerun"` approvals** (via the `ask` effect or
their standing tool mode). They never create `resolve="apply"` — that path exists
only for `request_config`'s grants, which a JS handler cannot reach (no DB handle,
no parent-side code). The agent has no runtime path to set its own tool modes or
policy; the human chooses the mode at approval time.

### Where policy is stored: tool envelope + per-agent registration

Two layers, composed — **neither agent-self-mutable**:

1. **Tool-authored policy** — the tool's intrinsic safety envelope, on
   `tools.policy` (static rules) or the `.qjs` policy fn. Travels with the tool,
   set by its author, audited at install. "send always asks" lives here.
2. **Tool-to-agent registration** — the binding of a tool to an agent, on the
   `grants` table (`kind='tool'`, `value=<toolname>`). This already carries
   `approval_mode` (the per-agent silent/always/tool_decides mode); per-agent
   **argument restriction** ("this agent may only use `action` in `[read,search]`")
   is the same kind of operator-set config and lives on the same binding.

The effective decision is the **most restrictive** of the two (the restrict-only
rule the dispatch gate already uses). The presented schema (`tools.parameters_json`)
may optionally be *narrowed* per agent to match the binding's restriction so the
model isn't even shown a forbidden enum value — but enforcement is the call-time
composition, which the model cannot bypass by ignoring the schema.

This revises the earlier "one level, no per-agent override": there *is* a per-agent
layer, but it is set at registration by an operator (or a human-approved
`request_config` `apply`), **never self-service by the agent** — the surviving
principle is "an agent cannot loosen its own policy," not "there is only one level."

### Declaring a tool with policy

`parameters` and `policy` are plain JSON objects in the manifest's `tools[]`
entry. Do not pre-stringify them; the DB stores them as JSON text.

```json
{
  "name": "github",
  "description": "GitHub CLI operations",
  "parameters": {
    "type": "object",
    "properties": {
      "action": {"type": "string",
                 "enum": ["pr_list", "pr_create", "pr_merge",
                          "issue_list", "issue_create", "issue_comment", "repo_clone"]}
    },
    "required": ["action"]
  },
  "policy": {"rules": [
    {"match": {"action": "pr_merge"}, "effect": "deny"},
    {"match": {"action": "repo_clone"}, "effect": "allow"},
    {"match": {}, "effect": "allow"}
  ]},
  "handler": "github.qjs"
}
```

### Dispatch flow with policy

The policy pre-filter runs first; `allow`/`ask` then fall through to the existing
per-tool mode gate in `dispatch_tool_inner`. Policy never replaces the gate, it feeds it.

```
LLM calls github({"action": "pr_merge", "number": 123})
  → main process: lookup tool "github" in registry
  → main process: read the tool's policy (ToolEntry.policy_json), evaluate against args
  → effect = deny  → return "error: action 'pr_merge' denied by policy" (no fork)
  → effect = ask   → set this call's gate to ASK → existing approval_create(...,"rerun")
                     → park; human picks once/always/deny → frozen call re-runs on approve
  → effect = allow → fall through to the normal per-tool mode gate (dispatch_tool_inner)
                     → fork JS child, execute handler
                       → handler calls cclaw.exec("gh pr list --json ...")
                       → nested fork+exec /bin/sh inside same sandbox
                       → result flows back to LLM
```

Policy evaluation happens in C in the main process before forking. Zero cost for
denied calls. `ask` and `allow` both proceed to the same mode gate every tool
already passes — policy only decides eligibility, the gate decides approval.

## Minimal policy example: `notes`

The smallest end-to-end demonstrator of the policy layer: an `fs.*`-only tool
(no `cclaw.exec`, no network, no secrets) that exercises all three effects.
`delete` is blocked in C before any fork. (A development fixture, not a
shipping tool.)

Manifest entry:

```json
{
  "name": "notes",
  "description": "Manage plain-text notes. Actions: list, read, write, delete.",
  "parameters": {
    "type": "object",
    "properties": {
      "action": {"type": "string", "enum": ["list", "read", "write", "delete"]},
      "name":   {"type": "string", "description": "note filename (no path)"},
      "body":   {"type": "string", "description": "content for write"}
    },
    "required": ["action"]
  },
  "policy": {"rules": [
    {"match": {"action": "delete"}, "effect": "deny"},
    {"match": {"action": "write"},  "effect": "ask"},
    {"match": {},                   "effect": "allow"}
  ]},
  "handler": "notes.qjs"
}
```

`notes.qjs` (a function body; `args` is in scope):

```javascript
var dir = fs.cwd() + '/.notes/';
var a = args.action;
if (a === 'list')  return (fs.readdir(dir) || []).join('\n');
if (a === 'read')  return fs.readFile(dir + args.name);
if (a === 'write') { fs.writeFile(dir + args.name, args.body || ''); return 'ok'; }
return 'error: unknown action ' + a;   // delete never reaches here (policy denies)
```

## The GitHub tool in JS (needs `cclaw.exec`)

With the manifest entry above (extended with `repo`, `number`, `title`,
`body`, `base`, `labels`, `auto_merge` properties), `github.qjs` builds a `gh`
command per action and runs it. This handler depends on the unimplemented
`cclaw.exec`:

```javascript
var repo = args.repo ? ' -R ' + args.repo : '';
var q = JSON.stringify;
var cmds = {
  pr_list:       'gh pr list --json number,title,state,url' + repo,
  pr_view:       'gh pr view ' + args.number + ' --json title,body,state,url' + repo,
  pr_create:     'gh pr create --title ' + q(args.title || '') + ' --body ' + q(args.body || '')
                 + (args.base ? ' --base ' + args.base : '') + repo,
  pr_merge:      'gh pr merge ' + args.number + (args.auto_merge ? ' --auto' : ' --merge') + repo,
  issue_list:    'gh issue list --json number,title,state,url' + repo,
  issue_create:  'gh issue create --title ' + q(args.title || '') + ' --body ' + q(args.body || '')
                 + (args.labels ? ' --label ' + args.labels.join(',') : '') + repo,
  issue_comment: 'gh issue comment ' + args.number + ' --body ' + q(args.body || '') + repo,
  repo_clone:    'gh repo clone ' + (args.repo || '') + ' .',
  repo_view:     'gh repo view --json name,description,url' + repo
};
var cmd = cmds[args.action];
if (!cmd) return 'error: unknown action ' + args.action;
var r = cclaw.exec(cmd, {timeout: 30});
if (r.exit_code !== 0) return 'error (exit ' + r.exit_code + '): ' + r.stderr;
return r.stdout;
```

## Per-argument policy & irreversible actions: `email`

`email` is the sharpest test of the policy model because, unlike `tmux`, its
dangerous action has no escape hatch — the only way to send is `action:"send"` —
and the *risk lives in an argument value* (the recipient), not just the verb. It is
the worked example of the **static floor + JS ratchet** model.

Action surface: `read`, `search`, `draft`, `send`.

**Static floor** (`tools.policy`) — the part a human audits at install. The author
chooses the `send` default here, and `send → allow` is the deliberate, visible
fail-open choice that lets the JS ratchet silently allow safe recipients:

```javascript
policy: {                              // plain object; the floor, in data
  rules: [
    { match: { action: ["read","search","draft"] }, effect: "allow" }, // nothing leaves the box
    { match: { action: "send" },                     effect: "allow" }, // default: permitted; JS adds friction
    { match: {},                                     effect: "deny"  }  // default-deny catch-all
  ]
}
```

**JS ratchet** (in the `.qjs`) — pure, args-only, `max`'d with the floor, so it can
only *raise* restriction. This is where the recipient/domain logic the data grammar
can't express lives:

```javascript
function policy(args) {
  if (args.action !== "send") return "allow";                 // floor stands
  var to = String(args.to || "");
  if (/@(competitor|press)\.com$/i.test(to)) return "deny";   // never
  if (!/@mycompany\.com$/i.test(to))         return "ask";    // external → park & ask
  return "allow";                                             // internal → floor stands, silent
}
```

`max(floor, js)`: internal recipient → `allow` (silent send), external → `ask`
(park), competitor → `deny` — exactly "send matching-this silently, matching-that
park-and-ask." The pattern logic is in JS; it can never loosen below the floor; the
one auditable fail-open knob is the single `send → allow` line of data.

`draft` sits deliberately on the `allow` side: writing a draft is reversible and
stays local, so the agent composes freely; only the irreversible step is gated.

### The floor is the knob; the wrinkle is honest

- **Want safe recipients to send *silently*?** Floor = `send → allow` (above). The
  human accepted that worst case in one readable line; the JS only adds friction.
- **Want sends to *never* go without at least one approval** (the `transfer`/`pay`
  posture)? Floor = `send → ask`. Then `max(ask, anything) ≥ ask` — no JS return,
  however buggy or compromised, can produce a silent send.

You **cannot** have both "gated by default" (floor=ask) *and* "JS silently allows
safe ones" — silent-allow is de-escalation, which `max` forbids on purpose. A tool
that needs content-based silent-allow must set floor=`allow`, and that fail-open is
precisely what the install audit exists to surface. That is the boundary working,
not a gap.

Two adjuncts that need no policy code at all:

- **The approval prompt already shows the args.** `handle_approval_park` prints the
  approval's `args_json`, so a human approving a `send` sees the real
  `to`/`subject`/`body`. With a `send → ask` floor and no JS, the human is the
  recipient policy — often enough on its own.
- **Hard "never" can also live in the handler.** It can return an error string (a
  hard deny, no fork-back) but cannot *park* — so use it for "never," not "maybe";
  prefer the JS ratchet's `deny` so the decision stays in the audited policy slot.

### Open question: `ask` + "always" is unsafe for `send`

An `ask` effect uses the `rerun` approval, which offers the human **once /
always / deny**. "Always" flips the tool's mode to `silent` — so one "always" on
*send to Bob* authorizes *every future send, to anyone*, unprompted. For an
outward, irreversible action that is precisely the wrong default.

The model has no per-action "once-only" today, and the approval identity is the
`tool_call_id`, not `(tool, recipient)` — so "always" can't be scoped to a
recipient. Options, in rough order of effort:

1. **Author-declared `once_only` (or `effect:"ask_once"`)** — marks `send` so its
   approval prompt omits "always"; the gate keeps asking every call. Smallest
   change, safe, and generalizes to every irreversible outward action (`post`,
   `transfer`, `merge --auto`).
2. **Scope the standing grant to a matched arg** — `always` flips to silent only
   for calls whose `to` matches the approved value; a new recipient re-gates.
   Needs the approval row to remember a per-arg key (a schema change).
3. **Operator discipline** — document "never answer *always* to a `send`."
   Fragile.

Recommendation: option 1 is the minimal honest fix and is the open design item
to settle before `email` — or any send-capable tool — ships.

## Implementation Phases

### Phase 1: `cclaw.exec` (not yet implemented — unblocks composable tools)

- Add `cclaw_exec` native function to the JS tool child environment
- Implementation: `fork()` + `exec("/bin/sh", "-c", cmd)` from the JS child
- Apply same sandbox as shell_exec (inherit sandbox_profile from agent)
- Timeout via alarm/waitpid
- Return `{stdout, stderr, exit_code}` as JS object
- Scope: only available in tool handler context (not js_eval, not channels)

### Phase 2: Policy layer (implemented)

- The `policy` column exists on the `tools` table; carried on
  `ToolEntry.policy_json`, sourced from `tools_load_extension_tools` for JS
  tools and seeded rows for builtins
- Policy evaluation in C (`policy_eval` in `src/tool_policy.c`): one SQLite
  JSON1 query matches args against rules — runs in the dispatch gate
  (`src/dispatch.c`) before any fork
- Semantics: NULL/empty policy → ALLOW (tool already in the allowed set);
  malformed policy → POLICY_ERROR, call blocked; first matching rule wins
- Effects: `deny` → error to LLM (no fork); `allow` → fall through to normal
  per-tool mode gate; `ask` → set gate to ASK, taking the shipped
  `approval_create(..., "rerun")` + park path (human picks once/always/deny)
- Secret interpolation wired for JS dispatch: `{{SECRET:name}}` in arguments
  resolves in the parent before blob serialization (`src/dispatch.c` JS-tier
  path)

### Phase 3: `cclaw.callTool` (not yet implemented, deferred)

- Socketpair between parent and JS child
- Request/response protocol: `{tool, args}` → `{result}`
- Depth limit (4) enforced by parent
- Enables JS tools calling other JS tools or C tools (web_fetch, file_read)
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

- `cclaw.exec` inherits the agent's sandbox_profile sandbox — no privilege escalation
- Policy evaluation is in C, before JS runs — can't be bypassed from handler
- **The forked child has no DB handle** — config rows are unreachable from a
  handler or `cclaw.exec`, so a JS tool cannot mutate capability/trust config
  (see "Config & Authority Boundary"). It can only create `resolve="rerun"`
  approvals, never `resolve="apply"`.
- **Secret interpolation is wired for all SBX dispatch tiers.**
  `{{SECRET:name}}` resolution happens in the daemon parent's per-tier dispatch
  path (`src/dispatch.c`) *before* the RunToolReq blob is serialized — shell
  interpolates `command`, web and js interpolate `arguments`. The interpolated
  values cross to the broker in the blob; the broker/child never holds the
  master key. A JS-defined extension tool's args are interpolated via the
  SBX_JS tier path. `{{SECRET:name}}` in a handler file body resolves in the
  child at the `http_request()` boundary (specs/security.md, JS-Tier Secret
  Resolution).
- Handler code is workspace-scoped (agent authored) — same trust as a draft extension
- Network from `cclaw.exec` (if built) goes through the same `HTTP_PROXY`/`HTTPS_PROXY` loopback proxy and host allowlist as `shell_exec` ([shell-networking.md](shell-networking.md))
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
| Typed tool surface | Plugins register tools via SDK | Manifest-declared tool with JSON schema, `.qjs` handler |
| Shell access | exec tool (binary) | cclaw.exec inside handler (hidden from LLM) |
| Policy | exec approvals (binary+argPattern regex) | Structured JSON rules on typed args |
| Approval UX | allow-once / allow-always interactive | per-rule deny/allow; `ask` reuses the `rerun` approval path (human picks once/always/deny) |
| Composability | Lobster pipelines for multi-step | cclaw.callTool for tool-calls-tool |
| Gating granularity | Per-binary + arg regex | Per-argument-value on structured schema |

The CClaw model is simpler (no regex parsing of command strings) and more
precise (policy operates on typed JSON, not shell tokenization).

## When a typed tool beats a skill

OpenClaw exposes most capabilities as **skills**: a `SKILL.md` prompt plus a
`requires.bins` allowlist, with every command flowing through the one `exec`
tool under one exec policy. That is the right default *when the agent already has
shell* — a typed wrapper then adds maintenance for almost no security gain,
because the agent can call the underlying binary through `shell_exec` anyway.
CClaw can do the same. Fine-grained policy on a
tmux tool is pointless next to a general `shell_exec` grant: the agent just types
`tmux …` into the shell.

A typed JS tool earns its complexity only when at least one of these holds:

1. **Grant-in-isolation.** You want the agent to have this capability *without*
   general shell — a monitor that may read panes but not run arbitrary commands,
   an agent that may use `github` with no `gh`/network grant. A skill can't
   separate the capability from `exec`; a registry tool with its own
   per-`(agent,tool)` mode can. This is the strongest reason.
2. **The gated distinction is structural, not an escape hatch.** Policy on typed
   args is only as real as the narrowest action the agent can't bypass. It has
   teeth for `email.send` (the *only* way to send is the typed action) and none
   for `tmux.send-keys` (which types into a live shell — arbitrary exec no matter
   how the enum is sliced). Gate the boundary the tool actually enforces (read
   vs. mutate, draft vs. send), not a sub-action taxonomy the underlying binary
   lets the agent route around.
3. **Argument-value gating matters.** "Whom" / "which" is part of the risk and
   the value lives in a structured field the policy — or the human at approval
   time — can see: `send` *to whom*, `message_send` *in which channel*. A regex
   over a shell string can't do this precisely; a typed arg can.

If none hold, ship a skill. Choosing to *type* a tool should be driven by one of
the three above, not by "it would be neater as a tool." For `tmux` specifically,
only #1 applies (and only the read-vs-mutate split, since `send-keys` is an exec
escape hatch) — so type it only for a shell-less agent, and keep its policy to
that one boundary rather than a per-subcommand taxonomy.

## Candidate JS Tools (from OpenClaw skills catalog)

### Pure HTTP API — no CLI needed (best candidates for JS tools)

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

## Recommended First JS Tool: `github`

**Why github:**
1. Clean REST API — no CLI dependency, works everywhere
2. Clear action taxonomy — reads vs writes vs destructive ops
3. Obvious policy value — allow reads, gate merges/closes
4. Most developers already have a GitHub token
5. Tests the full design: schema, policy, `http_request`, secret interpolation

**What it proves:**
- JS tool with typed `action` enum works
- Policy layer catches destructive calls before execution
- `http_request` (method, headers, body) is sufficient for real work
- No `cclaw.exec` needed — validates the pure-API path first

**Second tool: tmux or himalaya**
- tmux proves the `cclaw.exec` path (local process, no API)
- himalaya proves a hybrid (could be API but CLI is practical)

Build github first (it needs only `http_request`), then tmux to validate
`cclaw.exec`.
