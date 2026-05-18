# Phase 3 — Autonomy Features

## Goal

CClaw becomes a proactive personal agent that can work independently on multiple tasks, wake itself on schedules, and manage a tree of sub-agents. Also: cross-compile and deploy to Pogoplug.

## Components

### 1. Heartbeats

The agent wakes itself on a configurable cadence and runs a turn with no user input.

**Mechanism:**
- Timer thread sleeps for `heartbeat_interval` (default: 30 minutes)
- On wake: inject a system message into the agent's session: `"[heartbeat] Check on your active tasks and report any updates."`
- Agent runs a normal turn — can use tools, check sub-agent status, send Telegram messages
- If agent has nothing to do, it responds briefly and goes back to sleep

**Config:**
```json
{
  "heartbeat": {
    "enabled": true,
    "every": "30m",
    "prompt": "[heartbeat] Check on your active tasks."
  }
}
```

**Interaction with Telegram:**
- Heartbeat responses are sent to the "home" chat (configurable chat_id)
- If the agent has nothing to report, it can choose not to send a message (tool: `skip_reply`)

### 2. Cron / Scheduled Tasks

The agent can schedule future wake-ups for specific tasks.

**Tool: `cron_set`**
```json
{
  "name": "cron_set",
  "parameters": {
    "task": "string — what to do when triggered",
    "schedule": "string — 'at:2026-05-20T09:00:00' or 'every:6h' or cron expression",
    "one_shot": "boolean — delete after first run (default: true)"
  }
}
```

**Tool: `cron_list`** — list active scheduled tasks
**Tool: `cron_remove`** — cancel a scheduled task

**Storage:** SQLite table:
```sql
CREATE TABLE cron_jobs (
    id          TEXT PRIMARY KEY,
    session_id  TEXT NOT NULL REFERENCES sessions(id),
    task        TEXT NOT NULL,
    schedule    TEXT NOT NULL,
    next_run_at TEXT NOT NULL,
    one_shot    INTEGER DEFAULT 1,
    created_at  TEXT NOT NULL
);
```

**Scheduler thread:** wakes every 60s, checks for due jobs, injects task message into the appropriate session.

### 3. Full Sub-Agents

Extend Phase 2's minimal spawn with:

- **Spawn depth > 1** — sub-agents can spawn their own sub-agents (configurable max depth, default: 2)
- **Sub-agent roles:** `orchestrator` (can spawn children, monitor them) vs `leaf` (executes, cannot spawn)
- **Result delivery:** when a sub-agent completes, inject a summary into the parent session as a tool result
- **Cancellation:** parent can kill a sub-agent via `cancel_agent(agent_id)`
- **Workspace isolation:** each sub-agent gets its own workspace subdirectory

**Sub-agent lifecycle:**
```
parent calls spawn_agent(task, role="leaf")
  → new session created (parent_session_id set)
  → new thread starts agent loop
  → sub-agent works (up to max_iterations)
  → on completion: write result to parent's pending queue
  → parent's next turn sees: "[sub-agent abc completed] Result: ..."
```

**Limits (configurable):**
- Max concurrent sub-agents per parent: 3
- Max total sub-agents system-wide: 10
- Max spawn depth: 2
- Max iterations per sub-agent: 30

### 4. Session Branching

Upgrade from Phase 1's unidirectional stack to full tree navigation:

- **fork(entry_id)** — set leaf to entry_id, future appends branch off
- **navigate(entry_id)** — switch to a different branch, generate branch summary
- **branch_summary** — LLM-generated summary of the branch being left (injected as entry on the new branch)
- **list_branches()** — show all branches in current session

This enables the agent (or user) to explore alternatives, backtrack, and curate conversation history.

### 5. Compaction

When context approaches the model's limit:

1. Detect: count tokens in current branch (approximate: chars / 4)
2. Summarize: call LLM with "summarize this conversation so far" using the oldest N messages
3. Replace: delete old entries, insert compaction entry with summary
4. Continue: next turn uses compaction summary + recent messages

**Trigger:** when estimated tokens > `context_window * 0.8`

### 6. Cross-Compile + Pogoplug Deploy

- musl-cross toolchain for ARMv5TE: `arm-linux-musleabi-gcc`
- Static linking: single binary, no shared libs needed on device
- Makefile target: `make cross` produces `build/cclaw-arm`
- Deploy script: `scp build/cclaw-arm pogoplug:/usr/local/bin/cclaw && ssh pogoplug 'systemctl restart cclaw'`
- Systemd service file for auto-start
- Config file on device: `/etc/cclaw/config.json`
- DB on device: `/var/lib/cclaw/cclaw.db`

### Beads Breakdown

| Bead | Description |
|------|-------------|
| P3-heartbeat | Timer thread + heartbeat injection + skip_reply tool |
| P3-cron | cron_set/list/remove tools + scheduler thread + SQLite table |
| P3-subagent-full | Depth > 1, roles, result delivery, cancellation |
| P3-branching | fork, navigate, branch_summary, list_branches |
| P3-compaction | Token estimation + LLM summarization + entry replacement |
| P3-cross-compile | musl toolchain setup + Makefile cross target |
| P3-deploy | Deploy script + systemd service + device config |

## Open Questions

1. Should heartbeat frequency be adaptive? (More frequent when tasks are active, less when idle)
2. Should sub-agent results be delivered immediately (interrupt parent's current turn) or queued for next turn?
3. Compaction model — use the same model as the conversation, or a cheaper/faster one?
4. Pogoplug has 128MB RAM — what's the max number of concurrent sessions/sub-agents feasible?
5. Should branching be exposed as a tool (agent can branch itself) or only as a user command?
