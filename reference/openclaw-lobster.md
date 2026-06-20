# OpenClaw Lobster — Workflow Tool Reference

## What It Is

Lobster is an in-process workflow runtime for OpenClaw. It lets an agent execute multi-step pipelines as a single tool call, with built-in approval gates and resume tokens.

It is an optional plugin tool (not enabled by default). Enable with `tools.alsoAllow: ["lobster"]`.

## The Problem It Solves

Without Lobster, a multi-step workflow costs N tool calls, N round-trips to the LLM, and the model must orchestrate every step. Each step burns tokens and introduces non-determinism. There's also no clean way to pause a workflow for human approval and resume later.

## What It Buys You

### 1. Deterministic Pipelines (One Tool Call, Many Steps)

Instead of the LLM deciding step-by-step:
```
LLM → tool call 1 → LLM → tool call 2 → LLM → tool call 3
```

You get:
```
LLM → lobster(pipeline) → [step1 | step2 | step3] → result
```

The pipeline runs in-process. No extra LLM round-trips between steps.

### 2. First-Class Approval Gates

Any step can be marked `approval: required`. When hit:
- Execution pauses
- A `resumeToken` is returned to the model/user
- Nothing after the gate runs until a human explicitly approves

This is how you get "draft email → human approves → send" without building your own state machine.

### 3. Resumable State

Resume tokens are durable. The workflow doesn't re-run earlier steps on resume — it picks up exactly where it paused. This means:
- No re-fetching data that was already collected
- No re-running side effects that already succeeded
- Minimal token cost for the approval round-trip

### 4. Safety Enforcement by the Runtime

The Lobster runtime enforces:
- **Timeouts** (`timeoutMs`, default 20s)
- **Output caps** (`maxStdoutBytes`, default 512KB)
- **Sandbox awareness** (disabled in sandboxed contexts)
- **No secrets management** (delegates auth to the tools it calls)

These are enforced by the runtime, not by each script — you can't accidentally write a step that runs forever or dumps unbounded output.

### 5. Auditable Data Pipelines

Pipelines are data (YAML/JSON or pipe syntax), not arbitrary code. This means:
- Easy to log, diff, and replay
- Constrained grammar reduces LLM "creativity" in execution paths
- Validation is realistic because the surface is small

## The Two Interfaces

### Pipe Syntax (inline)
```json
{
  "action": "run",
  "pipeline": "inbox list --json | inbox categorize --json | approve --preview-from-stdin --prompt 'Send drafts?'"
}
```

### Workflow Files (.lobster)
```yaml
name: inbox-triage
steps:
  - id: collect
    command: inbox list --json
  - id: categorize
    command: inbox categorize --json
    stdin: $collect.stdout
  - id: gate
    command: inbox apply --approve
    stdin: $categorize.stdout
    approval: required
  - id: execute
    command: inbox apply --execute
    stdin: $categorize.stdout
    condition: $gate.approved
```

## The Approval Flow

```
Agent calls lobster(action=run, pipeline=...)
  → Steps execute until an approval gate
  → Returns: { status: "needs_approval", resumeToken: "..." }

Human reviews the preview, decides yes/no

Agent calls lobster(action=resume, token=..., approve=true)
  → Remaining steps execute
  → Returns: { status: "ok", output: [...] }
```

## What This Means for CClaw

Lobster fills a gap that CClaw doesn't currently have a primitive for: **gated side effects with durable pause/resume**. The CClaw equivalents would be:

| Lobster concept | CClaw analog (if any) |
|-----------------|----------------------|
| Pipeline steps | Sequential tool calls in advance_session() |
| Approval gate | Nothing — would need a new entry type or session state |
| Resume token | Nothing — session replay from entries is possible but not purpose-built |
| Output envelope | Tool result JSON |
| Timeout/caps | setrlimit on forked children, but not on multi-step sequences |

If CClaw wanted this, the minimal design would be:
- A tool that runs a sequence of sub-tool-calls
- A special "awaiting_approval" session state that parks the turn
- A resume mechanism (new entry type or CLI flag) that continues from the parked state
- Runtime-enforced limits on the entire sequence

The key insight: Lobster exists because the LLM-as-orchestrator pattern is expensive and non-deterministic for known workflows. Moving orchestration to a typed runtime saves tokens and adds safety gates that the model can't bypass.
