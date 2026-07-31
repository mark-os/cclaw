---
name: cclaw-agents
description: Working with the multi-agent roster — delegating via launch_agent/check_session, how sessions bind to agents and channels, and when to propose a new agent (see the creating-agents skill for the how).
---

# CClaw Agents

CClaw is multi-agent: each agent has its own name, system prompt, workspace
directory, grants, extensions, and sessions.

## The roster

The agent roster (names + descriptions) appears in `search_config` — check it
before delegating or proposing a new agent. Agents are isolated: sessions,
memory, and workspace are scoped by agent name.

## Delegating

`launch_agent {task, name?, tools?, background?}` — `task` is the whole brief:
the child starts with an empty transcript and sees only that text, never your
conversation.

- **`name`** — a roster agent. It runs under *its own* operator-configured
  grants, unfiltered; you cannot narrow them from the call site (`tools` with
  `name` is an error). Omit `name` to **self-spawn a worker**: your agent,
  your grants, narrowed by a session tool filter.
- **`tools`** — self-spawn only. A JSON array intersected with your own
  grants; it can only narrow, never widen. Omitted, the worker gets the
  default worker toolset, which `launch_agent`'s own tool description spells
  out by name — read it before assuming a worker can do something.
- **`background`** — default `false`: the call **blocks** and the tool result
  *is* the child's final answer. With `background:true` it returns
  immediately with `session_id=N` and the child's result arrives later in your
  inbox.

`check_session {session_id}` — a delegated session's state, plus its final
text once it reaches `idle`. Only your own direct children are visible.

Sub-agents nest: a worker can delegate again, bounded by `agent_max_depth`
(and per-parent / system-wide caps). If a worker reports `blocked by this
session's tool filter`, it is not missing a grant — reissue the spawn with an
explicit `tools` array naming what it needs (still within your own grants).
Running as a worker yourself, `search_config` prints a `session tool filter:`
line: your callable tools are your grants intersected with that list.

A session's agent is fixed at creation — there is no reassignment. Work that
should move to another agent is a new `launch_agent` session, not a transfer.

## Sessions and channels

A session's channel binding (`channel_name`/`chat_id`) is its origin — set
when an inbound channel message creates the session, or attached by the
operator. It is where approval prompts and turn output go. Agents don't
rebind sessions; what an agent *can* request is a **send route** to a chat
(`request_config` request_changes `routes` section), which authorizes
`channel_send` to that target.

## Creating new agents

If the operator keeps asking for the same specialized, recurring task, offer
to create a dedicated agent for it. The full schema, creation caps, and the
escalate-then-delegate two-step live in the **creating-agents** skill.
