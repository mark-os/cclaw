# Pi Agent — Extensions, Skills & Tools Reference

## Tool System

### Tool Definition Interface
```typescript
{
  name: string,              // snake_case, used in LLM tool calls
  label: string,             // human-readable for UI
  description: string,       // sent to LLM
  promptSnippet?: string,    // one-line for "Available tools" in system prompt
  promptGuidelines?: string[], // bullets appended to system prompt
  parameters: TSchema,       // TypeBox schema (JSON Schema compatible)
  executionMode?: "sequential" | "parallel",
  
  execute(toolCallId, params, signal, onUpdate, ctx): Promise<AgentToolResult>,
  renderCall?(...): Component,   // custom TUI rendering
  renderResult?(...): Component,
}
```

### Tool Result Format
```typescript
{
  content: (TextContent | ImageContent)[],  // returned to model
  details: T,                                // structured data for UI/logs
  terminate?: boolean,                       // hint to stop after this batch
}
```

### Built-in Tools
| Tool | Description | Default Active |
|------|-------------|----------------|
| `read` | Read file contents | ✅ |
| `bash` | Execute shell commands | ✅ |
| `edit` | Targeted text replacements | ✅ |
| `write` | Create/overwrite files | ✅ |
| `grep` | Search file contents (ripgrep) | ❌ |
| `find` | Find files by name (fd) | ❌ |
| `ls` | List directory contents | ❌ |

### Tool Dispatch Flow
1. LLM returns tool call in assistant message
2. `prepareToolCall()`: find tool, validate args via TypeBox, call `beforeToolCall` hook
3. If not blocked → `executePreparedToolCall()`: call `tool.execute()`, stream partial updates
4. `finalizeExecutedToolCall()`: call `afterToolCall` hook for result modification
5. Emit `tool_execution_end`, create `ToolResultMessage`

### Tool Output Truncation
- Line limit: 2000 lines
- Byte limit: 50KB
- Grep line length: 500 chars max per match
- Bash: truncate tail (errors at end)
- File read: truncate head

### External Tool Binaries
Pi auto-downloads `fd` and `rg` from GitHub releases if not in PATH. Stored in `~/.pi/bin/`. Offline mode via `PI_OFFLINE=1`.

---

## Extension System

### Extension = Factory Function
```typescript
type ExtensionFactory = (pi: ExtensionAPI) => void | Promise<void>;

export default function(pi: ExtensionAPI) {
  pi.registerTool(myTool);
  pi.registerCommand("mycommand", handler);
  pi.on("tool_call", handler);
}
```

### Discovery Locations (in order)
1. Project-local: `<cwd>/.pi/extensions/`
2. Global: `~/.pi/extensions/`
3. Explicitly configured: `settings.extensions[]`

### Discovery Rules
- Direct files: `extensions/*.ts` or `*.js`
- Subdirectory with index: `extensions/*/index.ts`
- Subdirectory with `package.json` containing `"pi"` field

### Package.json Manifest
```json
{
  "pi": {
    "extensions": ["src/index.ts"],
    "themes": ["themes/dark.json"],
    "skills": ["skills/"],
    "prompts": ["prompts/"]
  }
}
```

### Loading
Uses `jiti` (JIT TypeScript transpiler) for `.ts` extensions at runtime. Extensions can import from framework packages.

### Extension Capabilities
- Register tools (`pi.registerTool`)
- Register commands (`pi.registerCommand`)
- Hook into events (tool_call, tool_result, message_end, context, etc.)
- Register message renderers
- Register keyboard shortcuts
- Register flags
- Access event bus (`pi.events`)
- Control active tools (`pi.getActiveTools()`, `pi.setActiveTools()`)

---

## Event/Hook System

### Extension Events (Full List)

**Session Lifecycle:**
| Event | Purpose |
|-------|---------|
| `session_start` | Session loaded/created |
| `session_before_switch` | Before switching (cancellable) |
| `session_before_fork` | Before forking (cancellable) |
| `session_before_compact` | Before compaction (cancellable, can provide custom result) |
| `session_compact` | After compaction |
| `session_shutdown` | Before teardown |
| `session_before_tree` | Before tree navigation (cancellable) |
| `session_tree` | After tree navigation |

**Agent Lifecycle:**
| Event | Purpose |
|-------|---------|
| `before_agent_start` | After user prompt, before loop (inject messages, modify system prompt) |
| `agent_start` | Loop started |
| `agent_end` | Loop ended |
| `turn_start` | Turn started |
| `turn_end` | Turn ended |

**Message Events:**
| Event | Purpose |
|-------|---------|
| `message_start` | Message begins |
| `message_update` | Streaming token update |
| `message_end` | Message complete (can modify) |

**Tool Events:**
| Event | Purpose |
|-------|---------|
| `tool_call` | Before execute (can block, can mutate input) |
| `tool_result` | After execute (can modify result) |
| `tool_execution_start` | Execution begins |
| `tool_execution_update` | Partial output |
| `tool_execution_end` | Execution complete |

**Provider Events:**
| Event | Purpose |
|-------|---------|
| `before_provider_request` | Modify LLM request payload |
| `before_provider_payload` | Modify raw provider payload |
| `after_provider_response` | Inspect response headers/status |
| `context` | Modify message array before LLM call |

**Input Events:**
| Event | Purpose |
|-------|---------|
| `input` | User input received (can transform) |
| `user_bash` | User executed `!command` (can intercept) |

**Model Events:**
| Event | Purpose |
|-------|---------|
| `model_select` | Model changed |
| `thinking_level_select` | Thinking level changed |

**Resource Events:**
| Event | Purpose |
|-------|---------|
| `resources_discover` | Provide additional skill/prompt/theme paths |

### Event Bus (Inter-Extension)
```typescript
pi.events.emit(channel, data)
pi.events.on(channel, handler) → unsubscribe
```

---

## Approval/Permission System

### beforeToolCall Hook
```typescript
beforeToolCall(context, signal) → { block?: boolean, reason?: string } | undefined
```
- Called after argument validation, before execution
- Return `{block: true, reason}` → error tool result sent to LLM
- Extensions subscribe via `tool_call` event

### afterToolCall Hook
```typescript
afterToolCall(context, signal) → { content?, details?, isError?, terminate? } | undefined
```
- Called after execution, before events emitted
- Can replace content, change error status, signal termination

### Input Mutation
`tool_call` event allows mutating `event.input` in place (no re-validation).

### No Filesystem Sandboxing
Tools operate on real filesystem with process permissions. `cwd` scopes relative paths but doesn't restrict absolute.

### Tool Allowlist
```bash
pi --tools read,bash,edit    # Only these enabled
pi --no-tools                # No tools
```

---

## Skills System

### Skill Definition (Markdown + YAML frontmatter)
```markdown
---
name: my-skill
description: Short description for model
disable-model-invocation: false
---

# Skill Instructions

Content the agent reads when skill is invoked...
```

### Discovery
- Global: `~/.pi/skills/`
- Project: `<cwd>/.pi/skills/`
- Extension-provided paths
- Honors `.gitignore`, `.ignore`, `.fdignore`

### Discovery Rules
- Directory with `SKILL.md` → skill root (no further recursion)
- Otherwise: direct `.md` children
- Recurse subdirectories for `SKILL.md`

### System Prompt Integration
```xml
<available_skills>
  <skill>
    <name>my-skill</name>
    <description>Does something</description>
    <location>/path/to/SKILL.md</location>
  </skill>
</available_skills>
```
LLM instructed to use `read` tool to load skill file when task matches.

### Invocation
- User: `/skill:name additional instructions`
- Content wrapped in `<skill name="..." location="...">` XML
- Sent as user message

### Model Invocation Control
- `disable-model-invocation: false` → appears in system prompt, LLM can discover
- `disable-model-invocation: true` → only via `/skill:name` command

---

## Custom Tool Creation

### Via Extension File
Create `.ts` in `.pi/extensions/`:
```typescript
import { Type } from "typebox";

export default function(pi) {
  pi.registerTool({
    name: "my_tool",
    label: "My Tool",
    description: "Description for LLM",
    parameters: Type.Object({
      input: Type.String({ description: "Input" }),
    }),
    async execute(toolCallId, { input }, signal, onUpdate, ctx) {
      return {
        content: [{ type: "text", text: `Result: ${input}` }],
        details: {},
      };
    },
  });
}
```

### Via SDK (Programmatic)
```typescript
createAgentSession({
  customTools: [myToolDef],
  tools: ["read", "bash", "my_tool"],
});
```

### Runtime Tool Control
```typescript
pi.getActiveTools(): string[]
pi.setActiveTools(["read", "bash", "my_tool"]): void
pi.getAllTools(): ToolInfo[]
```

---

## MCP Integration

No native MCP built-in. Available as external extension package (`npm:pi-mcp-adapter`). MCP support is loaded via the extension system, not core — same approach CClaw will take (provide a reference MCP extension via `js_define_tool` or native C extension).

---

## Prompt Templates

File-based templates in `.pi/prompts/`. Invoked as `/templatename args`.
- `$@` for all args
- `$1`, `$2` for positional

---

## CClaw Mapping

| Pi Concept | CClaw Equivalent |
|------------|-----------------|
| Extension factory | `tool_*_register()` functions |
| TypeBox schema | JSON Schema string in `ToolSchema.parameters_json` |
| `beforeToolCall` hook | Could add pre-dispatch callback |
| `afterToolCall` hook | Could add post-dispatch callback |
| Skills (markdown) | Agent config files in `agents/` dir |
| Event bus | Not needed (single-threaded, no plugins) |
| Tool allowlist | Could add to agent config |
| `onUpdate` streaming | `cli_progress` callback |
| MCP | Not planned |
| Extension loading | MicroQuickJS `js_define_tool` (runtime tool creation) |
