---
name: creating-agents
description: Design and create a purpose-built agent with create_agent — the full definition schema, grant caps, clone_from, the escalate-then-delegate two-step, and packaging agents in extensions. CClaw is built to be a good agent creator; start here when a task deserves its own agent.
---

# Creating Agents

Creating a dedicated agent is a first-class move, not an edge case. A narrow
agent with a tight sandbox profile and minimal grants is safer than widening
yourself, keeps your own context clean, and gives recurring work a stable
home (its own sessions, memory, workspace, and cron pulse).

## When to create one

- The operator keeps asking for the same specialized, recurring task —
  monitoring a feed, nightly reports, watching a repo.
- A task needs capabilities you'd rather not hold permanently yourself.
- Work should survive independently of your session (cron + channel routing).

Don't create agents for one-off work; use your own session or `launch_agent`
with no name (self-spawn worker). Check the roster in `search_config` first —
don't duplicate an existing agent.

## Before you build

"Make me a weather agent" names an outcome, not a shape. An agent is durable —
removal is an operator act — so pin down the shape before spending an approval
on it. The sequence that works:

1. **Ask once, with a proposal.** Batch what you need to know into one message
   and lead with the defaults you'd pick: "I'll build a Watcher that polls
   api.weather.gov each morning and messages this chat on warnings — want
   anything different?" One word approves it; a specific request needs no
   question at all.
2. **Prototype in your own workspace.** Draft the tool as an extension,
   request any grant you need *for yourself* (you'll bequeath it to the
   child), and test the handler with `js_eval` until it works.
3. **Then `create_agent`**, bundling the tested extension and proven grants —
   the approval the operator sees is for something that already works.

Each ask should arrive with work attached: a proposal, then a working
prototype, then a definition.

## The definition schema

One JSON schema everywhere an agent is defined (`create_agent` arguments,
`agents[]` in extension manifests, `agents/<name>/agent.json` seeds):

```json
{
  "name": "Watcher",
  "description": "Watches the NWS feed and alerts on warnings",
  "system_prompt": "You are…",
  "primary_model": "deepseek/deepseek-v4-flash",
  "secondary_model": null,
  "sandbox_profile": "standard",
  "grants": { "tools": ["web_fetch"], "hosts": ["api.weather.gov"],
              "read_paths": [], "write_paths": [] },
  "extensions": ["nws"],
  "memory_blocks": [ { "label": "persona", "description": "…", "value": "…" } ],
  "max_iterations": 25,
  "shell_timeout": 30,
  "clone_from": "Assistant"
}
```

Only `name` is required (PascalCase). Everything else defaults sensibly.
`clone_from` copies an existing agent's row, grants, and extensions first,
then overlays the fields you set — "like Assistant but with X" is one field.

Models accept `model` or `model@provider` (the id form pins the provider).

## Creation caps — and the two-step when you hit them

An agent you create can never exceed you:

- `sandbox_profile` ≤ yours in looseness (host > standard > restricted).
- Every grant ⊆ your own live grants. Exact values, no fuzzy matching.
- Extensions must be published, or owned by you.
- `clone_from` cannot launder capabilities — the source is capped the same way.

These are hard refusals, not clamps. **Creating an agent is delegation**
(subdividing authority you hold); **expanding authority is escalation** and
deliberately lives elsewhere. If the child needs a grant you lack, do the
two-step:

1. `request_config` action `request_changes` — request the grant for
   *yourself* (one document, one approval).
2. After it's granted, retry `create_agent` bequeathing it.

Batch step 1 well: one `changes` document can carry every grant the child
will need, plus a provider definition and your own model switch. See the
`configuring-cclaw` skill for the document dialect.

`create_agent` validates eagerly — a definition that would fail caps never
parks — then parks one operator approval enumerating every grant value, the
profile, extensions, and models. Give the approver a good `description`.

## Refining an agent you created

`update_agent {name, ...fields}` revises an existing agent — yourself, or one
you created (`agents.created_by`). Same approval gate and the same caps as
creation, applied against you:

- Fields overlay: `description`, `system_prompt` (full replacement text),
  `primary_model` / `secondary_model` (must resolve to registered models),
  `sandbox_profile` (never looser than yours), `max_iterations`,
  `shell_timeout`. Absent fields keep their value; unknown keys are errors.
- `grants` **adds** (each must be one you hold) — it never removes. Removing
  a grant is an operator act, and expanding *your own* grants stays with
  `request_config` (escalation, not delegation).

So a first version doesn't have to be perfect: ship the Watcher, watch a few
sessions, then `update_agent` the prompt. Include a `reason` — the approver
sees old behavior only through you.

## Working with the new agent

- `launch_agent {task, name:"Watcher"}` — start a session on it. It blocks and
  returns the agent's final answer as the tool result; `background:true`
  returns `session_id=N` instead and the result is delivered to you when the
  child finishes — do not poll it or schedule a check on it. A named agent
  always runs under its own grants — the `tools` filter is self-spawn only.
- `check_session {session_id}` — a delegated session's state, and its final
  text once idle. A live progress peek, not a way to wait.
- Channel access: the new agent sends only where a `channel_routes` row
  resolves to it. Routes are requested via `request_changes`
  (`routes: ["telegram:12345"]`) by the agent itself, or set by the operator.
- Every agent gets a disabled `heartbeat` cron job (a bare wake every 30
  minutes, no payload) it can later enable.

## Packaging agents in extensions

`extension.json` may declare `agents[]` (same schema; `system_prompt_file`
allowed). Use it when the agent belongs with tools/skills/config as one
installable bundle — `extension_promote` enumerates the whole manifest for
approval, caps enforced against you as the promoting agent. An `agents[]`
name that already exists is skipped, never overwritten.
