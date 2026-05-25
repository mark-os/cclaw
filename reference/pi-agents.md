# Pi Agent — Sessions, Trees & Agent Loop Reference

## Agent Loop

### Core Flow
```
OUTER LOOP (follow-ups):
  while true:
    INNER LOOP (tool calls + steering):
      while hasMoreToolCalls OR pendingMessages:
        1. Inject pending messages into context
        2. Stream assistant response via LLM
        3. If error/aborted → emit events, return
        4. Extract tool calls
        5. Execute tool calls (parallel or sequential)
        6. Append tool results
        7. Emit turn_end
        8. prepareNextTurn() → can swap context/model/thinking
        9. shouldStopAfterTurn() → early exit
        10. Poll getSteeringMessages() → set as pending
    
    Poll getFollowUpMessages()
    If none → break
    Else → set as pending, continue

  Emit agent_end
```

### No Iteration Limit
Pi has NO hard turn limit. Runs until:
- Assistant stops making tool calls AND no steering/follow-up messages
- `shouldStopAfterTurn()` returns true
- Error or abort

### Tool Execution Modes
- **Parallel** (default): All tool calls prepared sequentially, executed concurrently
- **Sequential**: Each prepared, executed, finalized before next
- Per-tool override: `tool.executionMode = "sequential"`

### Message Queues
- **Steering:** Injected after current turn's tool calls, before next LLM call
- **Follow-Up:** Processed only after agent would otherwise stop

---

## Session Tree Model

### Structure
Sessions are **append-only trees** stored as JSONL files. Each entry has `id` and `parentId`. A `leafId` pointer tracks current position.

### Entry Types
| Type | Purpose |
|------|---------|
| `message` | User/assistant/toolResult messages |
| `thinking_level_change` | Records thinking level switch |
| `model_change` | Records model switch |
| `compaction` | Summary replacing old messages |
| `branch_summary` | Summary of abandoned branch |
| `custom` | Extension state (NOT sent to LLM) |
| `custom_message` | Extension content (IS sent to LLM) |
| `label` | Named bookmark on an entry |
| `session_info` | Session name/metadata |
| `leaf` | Records leaf pointer changes |

### JSONL File Format
```
{"type":"session","version":3,"id":"...","timestamp":"...","cwd":"...","parentSession":"..."}
{"type":"message","id":"abc12345","parentId":null,"timestamp":"...","message":{...}}
{"type":"message","id":"def67890","parentId":"abc12345","timestamp":"...","message":{...}}
```
- Line 1: Session header
- IDs: 8-char hex (truncated UUIDv7), collision-checked
- Append-only (entries never modified/deleted)

### Context Building (`buildSessionContext()`)
1. Walk from `leafId` to root via `parentId` → get path
2. Extract latest `thinkingLevel` and `model` from path
3. Find latest `compaction` entry in path
4. If compaction: emit summary + kept messages + post-compaction messages
5. If no compaction: emit all messages in path order
6. Branch summaries → user messages with summary prefix

---

## Branching & Navigation

### Branch (`branch(entryId)`)
Move `leafId` to earlier entry. Next append creates child of that entry → new branch. **No entries modified or deleted.**

### Branch with Summary
Same as branch + appends `BranchSummaryEntry` capturing context from abandoned path (LLM-generated summary).

### Tree Navigation (`navigateTree(targetId)`)
1. Collect entries unique to old branch
2. Optionally generate branch summary of abandoned path
3. Move leaf to target
4. If target is user message → return text for pre-filling input (edit/retry)

### Fork (`createBranchedSession(leafId)`)
Creates NEW session file with only the path from root to specified leaf. Links via `parentSession` in header.

### Undo
Navigate to parent of last user message → effectively undoes last exchange. Old branch remains.

---

## Context Window Management & Compaction

### Trigger
```
shouldCompact = contextTokens > contextWindow - reserveTokens
```
Checked after each LLM response using usage data.

### Default Settings
```
enabled: true
reserveTokens: 16384     // headroom before triggering
keepRecentTokens: 20000  // recent context preserved verbatim
```

### Algorithm
1. **Find cut point:** Walk backwards from end, accumulate token estimates. When >= `keepRecentTokens`, find nearest valid cut (user message, assistant message, branch summary — NOT tool results).
2. **Collect messages to summarize:** Everything before cut point.
3. **Extract file operations:** Read/write/edit from tool calls in summarized range.
4. **LLM summarization call:** Structured format (Goal, Constraints, Progress, Key Decisions, Next Steps, Critical Context).
5. **Store:** Append `CompactionEntry` with `{summary, firstKeptEntryId, tokensBefore, details}`.

### Stacking
Multiple compactions can stack. Each summarizes everything before it. Previous summary included in UPDATE prompt.

### Token Estimation
`chars / 4` heuristic for text. 4800 chars equivalent for images.

### Two Triggers in Practice
1. **Threshold** (normal): Exceeds budget. Compacts, does NOT auto-retry.
2. **Overflow** (emergency): LLM returns overflow error. Remove error, compact, auto-retry. Only once per overflow.

---

## Message Transformation

### Pipeline (per LLM call)
1. `transformContext(messages)` — prune, inject (extension hook)
2. `convertToLlm(messages)` — convert to LLM-compatible format

### Conversion Rules
| Internal Type | LLM Format |
|---------------|------------|
| user | user message |
| assistant | assistant message |
| toolResult | tool result |
| bashExecution | user message (formatted command/output) |
| custom | user message |
| branchSummary | user message (`<summary>...</summary>`) |
| compactionSummary | user message (`<summary>...</summary>`) |

### Cross-Provider Normalization
- Images → placeholder text for non-vision models
- Thinking blocks: same model → keep; different → text; redacted → drop
- Tool call IDs sanitized per provider
- Orphaned tool calls → synthetic error results
- Error/aborted messages → skipped

---

## Error Handling & Recovery

### Retryable Errors
```
overloaded|rate.?limit|429|500-504|service.?unavailable|
network.?error|connection.?refused|fetch failed|timeout|terminated
```

### Strategy
- Exponential backoff: `1s * 2^(attempt-1)`, max 3 retries
- Remove error message from agent state (keep in session history)
- Retry via `agent.continue()`
- Abortable during sleep
- Context overflow NOT retried (handled by compaction)

### Overflow Recovery
1. Detect via error message pattern matching
2. Remove error from state
3. Auto-compact
4. Auto-retry (once only)

---

## Built-in Commands

| Command | Description |
|---------|-------------|
| `/model` | Select model |
| `/compact` | Manual compaction |
| `/fork` | Create fork from previous message |
| `/clone` | Duplicate session at current position |
| `/tree` | Navigate session tree (switch branches) |
| `/name` | Set session display name |
| `/session` | Show session info/stats |
| `/export` | Export session (HTML/JSONL) |
| `/import` | Import from JSONL |
| `/new` | Start new session |
| `/resume` | Resume different session |
| `/settings` | Open settings |
| `/reload` | Reload extensions/skills/prompts |
| `/quit` | Exit |

---

## Progress & Streaming

### Agent Events
```
agent_start → turn_start →
  message_start → message_update* → message_end →
  tool_execution_start → tool_execution_update* → tool_execution_end →
turn_end → agent_end
```

### Tool Partial Updates
Tools stream progress via `onUpdate` callback during execution.

---

## Persistence

### Storage: JSONL Files (NOT SQLite)
- Each session = one `.jsonl` file
- Location: `~/.pi/agent/sessions/--<encoded-cwd>--/<timestamp>_<session-id>.jsonl`
- Append-only (most operations)
- Rewrite only for migrations or branched sessions
- Full file read on session open → in-memory index

### Deferred Write
Session file NOT created until first assistant message. Prevents empty files from abandoned prompts.

### In-Memory Index
```
fileEntries[]           // all entries
byId: Map<id, entry>   // lookup
labelsById: Map<id, label>
leafId: string | null   // current position
```

---

## Context Files (AGENTS.md)

### Loading
1. Check `~/.pi/agent/` for `AGENTS.md` or `CLAUDE.md`
2. Walk from CWD up to filesystem root, collecting from each ancestor
3. Deduplicate, order: global first, then root → cwd

### Injection
```xml
<project_context>
<project_instructions path="/path/to/AGENTS.md">
[content]
</project_instructions>
</project_context>
```

---

## AgentHarness (Orchestration Layer)

Sits between raw Agent loop and application. Handles:
- Session persistence (auto-save on message_end)
- Compaction orchestration
- Branch navigation with summarization
- Model/thinking level management
- Tool call/result hooks (delegated to extensions)
- Provider request lifecycle hooks

### Phases
`idle | turn | compaction | branch_summary | retry`

### prepareNextTurn
After each turn: flush writes, rebuild context, rebuild system prompt. System prompt, tools, and context refreshed between every turn.

---

## CClaw Mapping

| Pi Concept | CClaw Equivalent |
|------------|-----------------|
| JSONL session file | SQLite entries table (JSON data blob) |
| leafId pointer | `sessions.leaf_id` column |
| Entry parentId | `entries.parent_id` column |
| Compaction entry | Could be entry with `type="compaction"` |
| Branch summary | Could be entry with `type="branch_summary"` |
| No iteration limit | CClaw has `max_iterations` (configurable) |
| AGENTS.md loading | CClaw loads from workspace `SOUL.md`/`MEMORY.md` |
| Steering queue | Could map to inbox system |
| Follow-up queue | Could map to inbox system |
