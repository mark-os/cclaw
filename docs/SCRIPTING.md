# Scripting — MicroQuickJS Integration

## Why

C is not self-modifiable at runtime. Pi's agent can edit its own source and reload; CClaw cannot. Instead, CClaw embeds a JavaScript engine so the agent can:

1. **Execute code on the fly** — data processing, calculations, string manipulation, research assistance
2. **Define new tools at runtime** — agent writes a JS function that becomes a callable tool for the session
3. **Prototype before committing** — test logic in JS before asking to write it in C

## Why MicroQuickJS

| Property | Value |
|----------|-------|
| RAM | 10 KB minimum, ~50-100 KB typical |
| ROM | ~100 KB (ARM Thumb-2) |
| Author | Fabrice Bellard (QEMU, FFmpeg, QuickJS) |
| Language subset | ES2020 minus: WeakRef, FinalizationRegistry, SharedArrayBuffer, generators, async/await |
| Sandboxing | No filesystem, no network, no process access by default |
| Timeout | Instruction count interrupt hook — configurable CPU limit |
| Memory limit | Configurable heap cap |
| License | MIT |

Key advantage over Lua: LLMs write better JavaScript than Lua. The agent will be generating this code, so LLM fluency matters more than human ergonomics.

Key advantage over full QuickJS: 5-10x smaller memory footprint. Critical for 128MB Pogoplug running multiple sessions.

## Architecture

```
Agent Loop
  ├── shell_exec tool (Phase 1)
  ├── js_eval tool (Phase 2/3)
  └── js_define_tool tool (Phase 2/3)

js_eval:
  Agent writes JS code → MicroQuickJS executes → stdout captured → returned as tool result

js_define_tool:
  Agent writes a JS function → registered in tool registry → available for future turns
```

## Tools

### js_eval

```json
{
  "name": "js_eval",
  "description": "Execute JavaScript code and return the result. Has access to: console.log(), JSON, Math, String/Array/Object methods. No filesystem or network access.",
  "parameters": {
    "code": "string — JavaScript code to execute"
  }
}
```

Execution:
1. Create MicroQuickJS context (or reuse session-persistent one)
2. Set memory limit (1 MB) and instruction limit (10M instructions ≈ ~1s on ARM)
3. Redirect `console.log` to output buffer
4. Evaluate code
5. Return: last expression value (if non-void) + console output
6. On timeout/OOM: return error result, context is still valid for next call

### js_define_tool

```json
{
  "name": "js_define_tool",
  "description": "Define a new tool by providing a JavaScript function. The tool becomes available for the rest of this session.",
  "parameters": {
    "name": "string — tool name (snake_case)",
    "description": "string — what the tool does",
    "parameters_schema": "object — JSON Schema for the tool's parameters",
    "code": "string — JavaScript function body. Receives 'args' object. Return a string result."
  }
}
```

Example agent usage:
```
I'll define a tool to count words in a file:

js_define_tool({
  name: "word_count",
  description: "Count words in text",
  parameters_schema: {"type": "object", "properties": {"text": {"type": "string"}}},
  code: "return args.text.split(/\\s+/).filter(w => w.length > 0).length.toString();"
})
```

After this, `word_count` appears in the tool list for subsequent turns.

## Session-Persistent JS Context

Each session maintains a MicroQuickJS context that persists across turns:

- Global variables survive between `js_eval` calls
- Defined tools remain registered
- Context is destroyed when session ends
- Context is NOT persisted to SQLite (recreated on session reload by re-running `js_define_tool` entries from history)

On session load from SQLite: scan entries for `js_define_tool` calls, replay them to rebuild the JS context.

## Sandboxing

MicroQuickJS provides strong sandboxing by default:

- **No I/O** — no `fs`, `net`, `process`, `require`, `import`
- **No eval of external code** — only code passed through the tool
- **Memory cap** — configurable per-context (default 1 MB)
- **CPU cap** — instruction count hook, kills execution after limit
- **No shared state** — each session has its own isolated context

What we expose to JS:
- `console.log()` → captured to output buffer
- `JSON.parse()`, `JSON.stringify()`
- Standard built-ins: Math, String, Array, Object, Map, Set, RegExp, Date
- A `cclaw` object with read-only session info (session_id, turn_number)

What we do NOT expose:
- File system access (use file_read/file_write tools instead)
- Network access (use shell_exec + curl instead)
- Process spawning
- Timer functions (setTimeout, setInterval)

## Build Integration

MicroQuickJS is a few C files. Vendor them like cJSON:

```
vendor/
  mquickjs/
    mquickjs.c
    mquickjs.h
    mquickjs-libc.c  (stripped — only the safe subset)
```

Makefile addition:
```makefile
MQUICKJS_SRC = vendor/mquickjs/mquickjs.c
CFLAGS += -DCONFIG_MQUICKJS
```

## Phase Placement

- **Phase 1:** No scripting. shell_exec only.
- **Phase 2:** `js_eval` tool available. Basic execution, no persistent context.
- **Phase 3:** `js_define_tool` + persistent context + tool replay on session load.

This is a progressive enhancement — the agent works fine without it, but gains significant capability with it.

## Risks

1. **MicroQuickJS is new (Dec 2025)** — less battle-tested than QuickJS. Mitigation: the subset is small, Bellard's track record is excellent.
2. **ARM Thumb-2 vs ARMv5TE** — MicroQuickJS targets Thumb-2 (ARMv7+). May need patches for ARMv5. Mitigation: test early on Pogoplug, fall back to full QuickJS if needed.
3. **Context memory growth** — persistent contexts accumulate state. Mitigation: cap at 1MB, destroy and recreate if exceeded.
