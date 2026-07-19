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

- `launch_agent {agent, prompt}` — start a session on another agent; returns
  a session id. Omit `agent` to self-spawn a worker with your own identity.
- `check_session {session_id}` — poll a delegated session's state and read
  its latest output. Only your own children are visible.

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
