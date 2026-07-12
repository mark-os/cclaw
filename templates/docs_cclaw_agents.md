---
name: cclaw-agents
description: The agent-definition schema, creating agents with create_agent, launching them with launch_agent, sandbox profiles and grant caps — and when to propose creating a new agent.
---

# CClaw Agents

CClaw is multi-agent: each agent has its own name, system prompt, workspace
directory, grants, extensions, and sessions. You can create new agents
(operator-approved) and delegate work to them.

## When to propose creating an agent

If the operator keeps asking for the same specialized, recurring task —
monitoring a feed, nightly reports, watching a repo — offer to create a
dedicated agent for it. A narrow agent with a tight profile and minimal grants
is safer and keeps your own context clean. Don't create agents for one-off
work; use your own session.

## Agent-definition schema

One JSON schema is used everywhere an agent is defined (the `create_agent`
tool, `agents[]` in extension manifests, `agents/<name>/agent.json` seeds):

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

Only `name` is required (PascalCase). `clone_from` copies an existing agent's
row, grants, and extensions first, then overlays the fields you set.

## Creation caps

An agent you create can never exceed you:

- `sandbox_profile` must be ≤ yours in looseness
  (host > trusted > standard > restricted).
- Every grant must be a subset of your own live grants.
- Extensions must be published, or owned by you.
- `clone_from` cannot launder capabilities: the clone source is capped the
  same way.

`create_agent` validates eagerly (bad definitions fail immediately) and then
parks an operator approval summarizing the profile and grant counts.

## Working with agents

- `launch_agent {agent, prompt}` — start a session on another agent; returns a
  session id.
- `check_session {session_id}` — poll a delegated session's state and read its
  latest output.
- The agent roster (names + descriptions) appears in `search_config` — check
  it before creating a duplicate.
