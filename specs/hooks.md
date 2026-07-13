# Hooks — Lifecycle Event Dispatch

Status: design. Captures the hook points, their inputs/outputs, and how they
compose with the existing advance loop.

---

## 1. Motivation

Agents need to intercept and augment the turn cycle without modifying core C
code. Current state:

| Hook | Status | Can modify |
|------|--------|-----------|
| `beforeToolCall` (gate) | ✅ Landed | restrict only (deny/ask/allow) |
| `afterToolCall` (observe) | ✅ Landed | nothing (side-effect only) |
| `beforeRequest` | 🗑️ Deleted (was dead code, never wired) | was: full messages array |
| `afterResponse` | ❌ Spec'd in extensions.md, not built | — |
| `turnStart` / `turnEnd` | ❌ Spec'd, not built | — |

The dead `beforeRequest` attempted to hand the hook the entire serialized
messages array, let JS mutate it, and return a replacement body. Problems:

1. Duplicates what `llm_payload.c`'s SQL already does.
2. Hook must parse/rebuild a potentially huge array.
3. Return value bypasses the SQL payload builder — two paths to the same output.

This design replaces that approach with **structured command hooks**: small JSON
in, targeted JSON commands out. The hook never sees or rebuilds the full
context.

---

## 2. Hook Events (revised)

Six events are specified; **four are dispatched** (`preAdvance`, `postAdvance`,
`beforeToolCall`, `afterToolCall`). `turnStart`/`turnEnd` are design headroom —
not yet wired, and `extension_manifest_validate` **rejects** manifests that
declare them (a registered hook that never fires is a silent lie). Each hook
handler is a JS file evaluated in a fresh QJS context per dispatch. A handler that throws is skipped (logged); one broken hook never kills
the turn.

| Event | When it fires | Input (JSON) | Output (JSON) | Chaining |
|-------|---------------|--------------|---------------|----------|
| `turnStart` | Entry to advance loop | `{session_id, agent}` | — (informational) | all run |
| `preAdvance` | After context plan built, before `llm_payload.c` | see §3 | commands (see §4) | chained in load order |
| `postAdvance` | After LLM response written to entries | see §3 | commands (see §4) | chained in load order |
| `beforeToolCall` | After capability resolution, before exec | `{name, args}` | `{deny, ask, reason}\|{}` | most-restrictive wins |
| `afterToolCall` | After tool exec, result available | `{name, args, result}` | `{result}\|{}` | chained; each sees previous |
| `turnEnd` | Before advance loop exit | `{session_id, agent, stop_reason}` | — (informational) | all run |

**Changes from extensions.md:**
- `beforeRequest` → `preAdvance` (name reflects what it actually intercepts)
- `afterResponse` → `postAdvance` (symmetry; operates on DB entries, not raw HTTP)
- `afterToolCall` gains result-replacement (was observe-only in C; spec already
  had it — align implementation to spec)

---

## 3. Inputs

Hooks receive a **small, structured JSON object** — not the full messages array.
The C dispatch function builds this from DB state before calling into QJS.

### `preAdvance` input

```json
{
  "session_id": 42,
  "agent": "default",
  "entry_count": 17,
  "last_role": "tool",
  "last_entry_id": 204,
  "token_estimate": 8400,
  "turn_number": 5,
  "pending_tool_calls": 0
}
```

Enough context to decide *whether* to act, without paying serialization cost of
the full history. If a hook needs entry content, it can query via the `db`
global (read-only access to session entries — see §6).

### `postAdvance` input

```json
{
  "session_id": 42,
  "agent": "default",
  "entry_id": 205,
  "role": "assistant",
  "content": "Here's the file you requested...",
  "tool_calls": [{"id": "call_1", "name": "file_read", "args": "{...}"}],
  "stop_reason": "tool_use",
  "usage_in": 3200,
  "usage_out": 450
}
```

The response entry as-written. Content is the full text (hooks that need to
redact/transform must see it). Tool calls included when present.

---

## 4. Outputs (command pattern)

Hooks return a JSON object of **commands**. Unknown keys are ignored (forward
compat). An empty object `{}` or `void` means "no action." Commands are
additive — they compose across chained hooks.

### `preAdvance` commands

```json
{
  "inject": [
    {"role": "system", "content": "User timezone: America/Chicago", "ephemeral": true}
  ],
  "suppress": [14, 16],
  "pin": [3],
  "set_data": {"12": {"priority": "high"}}
}
```

| Command | Effect |
|---------|--------|
| `inject` | Insert transient entries before the LLM call. `ephemeral: true` means not persisted after this turn (excluded from future context). Role must be `system` or `user`. |
| `suppress` | Entry IDs to exclude from this request's context window (not deleted — just hidden for this call). |
| `pin` | Entry IDs that must appear in context even under token pressure (override compaction). |
| `set_data` | Patch the `data` JSON column on entries by ID. Merged via `json_patch`. |

**Constraints:**
- `inject` entries are system/user only (no faking assistant messages).
- `suppress` cannot hide the most recent user entry (the actual request).
- `pin` entries must belong to this session.
- All entry IDs are validated against the session before application.

### `postAdvance` commands

```json
{
  "transform_content": "Here's the file you requested [REDACTED]...",
  "annotate": {"reviewed": true, "category": "file_ops"},
  "inject_next": [
    {"role": "system", "content": "Reminder: check permissions", "ephemeral": true}
  ],
  "notify": {"channel": "slack", "message": "Agent wrote a file"}
}
```

| Command | Effect |
|---------|--------|
| `transform_content` | Replace the assistant entry's `content` in-place (DB UPDATE). Use for redaction/PII stripping. Original preserved in `data.original_content` if set. |
| `annotate` | Merge into the entry's `data` JSON column. |
| `inject_next` | Entries to prepend to the *next* advance cycle's context (ephemeral by default). Useful for injecting "you just did X, remember Y" steering. |
| `notify` | Fire-and-forget notification. Dispatcher routes by channel name to the channel outbox. |

**Constraints:**
- `transform_content` cannot change the role or tool_calls structure.
- `inject_next` is capped (max 3 entries, max 2048 chars total).
- `notify` is best-effort; failure doesn't error the hook.

### `afterToolCall` commands

```json
{
  "result": "modified tool output...",
  "annotate": {"redacted": true}
}
```

| Command | Effect |
|---------|--------|
| `result` | Replace the tool result content before it's written to entries. Chained — each hook in load order sees the previous hook's result. |
| `annotate` | Merge into the tool-result entry's `data` column. |

---

## 5. Execution Model

Hooks run in the **agent process** (not forked), in a fresh QJS context per
dispatch. This matches the existing `gate_tool_call` / `observe_tool_call`
pattern.

> **Correction (as built):** the diagram below is a simplification.
> `advance_session()` is a pure state machine; payload build + HTTP + ingest run
> in `llm_proc.c` on a **worker thread** with its own DB connection, while the
> hooks QJS runtime is main-poll-thread-only. So `preAdvance` fires on the main
> thread in `dispatch_llm_req()` (before worker submit) and its commands cross
> the thread boundary **as DB state** (`hook_directives` rows + `entries.data`
> flags), read by the worker's payload build and cleared at every `llm_req`
> exit. `postAdvance` fires on the main thread at the worker-completion wake,
> before `advance_session` consumes the result.

```
advance_session()
  ├─ build context plan
  ├─ dispatch preAdvance hooks   ← NEW
  │    └─ apply commands (inject/suppress/pin/set_data)
  ├─ llm_payload.c assembles request (reads entries table)
  ├─ LLM HTTP call (worker thread)
  ├─ parse response, write entries
  ├─ dispatch postAdvance hooks  ← NEW
  │    └─ apply commands (transform/annotate/inject_next/notify)
  ├─ if tool_use: dispatch beforeToolCall (existing)
  │    └─ exec tool
  │    └─ dispatch afterToolCall (existing, gains result replacement)
  └─ loop or exit
```

**Timing:** hooks are synchronous and blocking. A slow hook delays the turn.
`setrlimit` on CPU time applies to the QJS eval (same as existing hooks).
Future: add a per-hook `timeout_ms` field in the manifest (kill context on
exceed, skip hook, log warning).

**DB access:** hooks get a read-only `db.query(sql, ...params)` global that
can SELECT from `entries`, `sessions`, `config`, `memory_blocks` (the agent's own
data). No writes through this path — mutations happen only via the returned
command object, validated by C before application.

---

## 6. QJS Globals Available to Hooks

All hooks share a base set of globals (same fresh-context-per-dispatch model):

| Global | Type | Notes |
|--------|------|-------|
| `input` | object | The hook's input JSON (§3) |
| `db.query(sql, ...params)` | function → array | Read-only SELECT on agent-visible tables |
| `console.log(...)` | function | Captured to stderr / syslog |
| `kv.get(key)` / `kv.set(key, val)` | function | Per-extension persistent key-value (future extension-scoped table — not the global `config` table, which is registry-only) |

No `fs.*` (hooks run in-process, not sandboxed children — filesystem access is
a tool concern). No `http_fetch` (hooks must not block on network; use `notify`
command for async outbound).

---

## 7. Registration

Hooks are declared in the extension manifest (unchanged from extensions.md):

```json
{
  "hooks": [
    {"event": "preAdvance", "handler": "inject_context.js"},
    {"event": "postAdvance", "handler": "redact_pii.js"},
    {"event": "afterToolCall", "handler": "audit.js"}
  ]
}
```

Stored in `hooks` table (real schema — there is **no** `load_order` column;
"load order" is `ORDER BY extension_name, event, path`):

```sql
CREATE TABLE IF NOT EXISTS hooks (
  extension_name TEXT NOT NULL,
  event          TEXT NOT NULL,
  path           TEXT NOT NULL,
  enabled        INTEGER NOT NULL DEFAULT 1,
  PRIMARY KEY (extension_name, event, path)
);
```

`extension_load_hooks()` already loads these into `ExtensionCtx.hooks[]` at
turn start. The `HookEvent` enum gains `HOOK_PRE_ADVANCE` and
`HOOK_POST_ADVANCE`; dispatch functions follow the `gate`/`observe` pattern.

---

## 8. Chaining and Conflict Resolution

| Event | Multiple hooks | Resolution |
|-------|---------------|------------|
| `turnStart` / `turnEnd` | All run (order irrelevant) | — |
| `preAdvance` | Chained in load order | Commands accumulate. Conflicting `suppress` vs `pin` on same ID: `pin` wins. |
| `postAdvance` | Chained in load order | Last `transform_content` wins. `annotate` merges. `inject_next` accumulates (subject to cap). |
| `beforeToolCall` | Most-restrictive wins | deny > ask > allow |
| `afterToolCall` | Chained in load order | Each sees previous `result` |

---

## 9. Security Considerations

- **No write access to DB from hooks.** All mutations go through the command
  object, validated by C. A hook cannot `INSERT INTO entries` directly.
- **Ephemeral injection is capped.** `inject` and `inject_next` have a max
  entry count and char budget to prevent context flooding.
- **`suppress` cannot hide the triggering user message.** Prevents a hook from
  silently dropping the user's instruction.
- **`transform_content` preserves original.** Stored in `data.original_content`
  so redaction is auditable.
- **`kv` is extension-scoped.** One extension cannot read/write another's
  persistent state.
- **Hook source is loaded from the installed extension path** (not draft
  workspace). Only promoted extensions' hooks run. Draft hooks require explicit
  `--test-hook` invocation (future).

---

## 10. Migration from Dead Code

1. ~~Delete `hook_dispatch_before_request()` and `build_messages_json()` helper.~~ Done.
2. ~~Delete `test_hook_dispatch.c` tests that exercise `before_request`.~~ Done.
3. Add `HOOK_PRE_ADVANCE` and `HOOK_POST_ADVANCE` to `HookEvent` enum.
4. Implement `hook_dispatch_pre_advance()` and `hook_dispatch_post_advance()`
   following the command-return pattern.
5. Wire dispatch calls into `advance_session()` at the points shown in §5.
6. Update `afterToolCall` dispatch to consume `result` return and feed it
   forward (currently ignores return value).
7. Update extensions.md event table to match this doc.

---

## 11. Examples

### Inject RAG context before each LLM call

```javascript
// inject_context.js — preAdvance hook
var results = db.query(
    "SELECT content FROM memory_blocks WHERE agent_name=? ORDER BY updated_at DESC LIMIT 3",
    input.agent
);
if (results.length > 0) {
    var combined = results.map(function(r) { return r.content; }).join("\n---\n");
    return {
        inject: [{role: "system", content: "Relevant memory:\n" + combined, ephemeral: true}]
    };
}
return {};
```

### Redact secrets from assistant output

```javascript
// redact_pii.js — postAdvance hook
var patterns = [
    /sk-[a-zA-Z0-9]{20,}/g,
    /ghp_[a-zA-Z0-9]{36}/g,
    /AKIA[A-Z0-9]{16}/g
];
var content = input.content;
var changed = false;
for (var i = 0; i < patterns.length; i++) {
    if (patterns[i].test(content)) {
        content = content.replace(patterns[i], "[REDACTED]");
        changed = true;
    }
}
if (changed) {
    return {transform_content: content, annotate: {redacted: true}};
}
return {};
```

### Audit tool calls to channel outbox

```javascript
// audit.js — afterToolCall hook
if (input.name === "shell_exec" || input.name === "file_write") {
    return {
        notify: {channel: "telegram", message: input.agent + " called " + input.name}
    };
}
return {};
```

---

## 12. Open Questions

- **Should `preAdvance` see the context plan metadata** (which entries are
  planned for inclusion, estimated tokens per entry)? Useful for smart
  suppression but exposes internal context-window logic to hooks.
- **Should `afterToolCall` result replacement compose with the secret scanner?**
  Currently the scanner runs after `reap_children`. If a hook also transforms
  the result, ordering matters. Proposed: scanner runs first (security
  boundary), then hooks see the already-scanned result.
- **`notify` routing** — does it go through the channel outbox table (existing
  path), or a lighter sideband? Channel outbox requires a session_id; hook
  notifications are session-scoped so this works, but feels heavyweight for a
  log line.
- **Hook testing CLI** — `cclaw --test-hook <event> <handler.js> [input.json]`
  to dry-run a hook against synthetic input without running a real turn.
