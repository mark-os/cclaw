# Hooks — Lifecycle Event Dispatch

Status: implemented for four events (`preAdvance`, `postAdvance`,
`beforeToolCall`, `afterToolCall`). This file is the command contract. The
handler contract summary is in
[extensions.md](extensions.md#hook-handler-contract). Code:
`src/hook_dispatch.c`, `src/extension.c` (`extension_load_hooks`).

---

## 1. Design: structured command hooks

A hook intercepts the turn cycle without changing C code. A hook receives a
small JSON `input` object and returns a small JSON object of **commands**. C
validates each command and applies it with SQL. A hook never sees or rebuilds
the full messages array.

Rationale: an earlier `beforeRequest` hook received the whole serialized
messages array and returned a replacement body. It duplicated the SQL payload
builder (`llm_payload.c`), forced the hook to parse a large array, and created
a second path to the request body. It was never wired and is deleted.

---

## 2. Hook Events

Six events are specified; **four are dispatched** (`preAdvance`, `postAdvance`,
`beforeToolCall`, `afterToolCall`). `turnStart`/`turnEnd` are design headroom.
They are not dispatched, and `extension_manifest_validate` **rejects**
manifests that declare them (a registered hook that never fires is a silent
lie). A hook handler file holds one function expression. Each dispatch
evaluates it in a fresh QuickJS context and calls it with `input`. A handler
that throws is skipped; one broken hook never stops the turn.

| Event | When it fires | Input (JSON) | Output (JSON) | Chaining |
|-------|---------------|--------------|---------------|----------|
| `turnStart` *(not dispatched)* | Entry to advance loop | `{session_id, agent}` | — (informational) | all run |
| `preAdvance` | After context plan built, before `llm_payload.c` | see §3 | commands (see §4) | chained in load order |
| `postAdvance` | After LLM response written to entries | see §3 | commands (see §4) | chained in load order |
| `beforeToolCall` | After capability resolution, before exec | `{name, args}` | `{deny, ask, reason}\|{}` | most-restrictive wins |
| `afterToolCall` | After tool exec, result available | `{name, args, result}` | `{result}\|{}` | chained; each sees previous |
| `turnEnd` *(not dispatched)* | Before advance loop exit | `{session_id, agent, stop_reason}` | — (informational) | all run |

`preAdvance` replaces the deleted `beforeRequest`; `postAdvance` replaces the
never-built `afterResponse` and acts on the DB entry, not the raw HTTP body.

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

This is enough context to decide *whether* to act, without the cost of
serializing the full history. Hooks have no DB access (see §6).

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

Hooks return a JSON object of **commands**. Unknown keys are ignored. An empty
object `{}` or no return value means "no action". Commands compose across
chained hooks.

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
| `inject` | Add content before the LLM call. Role must be `system` or `user`. `ephemeral: true` writes a `hook_directives` row that applies to this request only. Without `ephemeral`, the content is appended as an entry. Injected content passes through the secret mask and scanner like any tool result. |
| `suppress` | Entry IDs to exclude from this request's context window (not deleted — just hidden for this call). |
| `pin` | Entry IDs that must appear in context even under token pressure. Protects the read-time cut only — compaction's keep boundary ignores pin, so a pinned entry old enough to compact is summarized like any other (see [memory.md](memory.md#context-window-management)). |
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
  "annotate": {"reviewed": true, "category": "file_ops"}
}
```

| Command | Effect |
|---------|--------|
| `transform_content` | Replace the assistant entry's `content` in place (DB UPDATE, `entries_fts` kept in sync). Use for redaction. The first transform stores the original in `data.original_content`. |
| `annotate` | Merge into the entry's `data` JSON column. |

**Constraints:**
- `transform_content` cannot change the role or the tool_calls structure.

Not implemented: `inject_next` (steering for the next request) and `notify`
(fire-and-forget message to a channel). A hook that returns them has no
effect.

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

Hooks run in the **trusted process** (not forked), on the main poll thread, in
a fresh QuickJS context per dispatch. `advance_session()` is a pure state
machine. Payload build, HTTP and ingest run in `llm_proc.c` on a **worker
thread** with its own DB connection. The hooks QuickJS runtime is
main-thread-only, so:

- `preAdvance` fires on the main thread in `dispatch_llm_req()`
  (`src/dispatch.c`), before the worker job is submitted. Its commands cross
  the thread boundary **as DB state**: `hook_directives` rows and
  `entries.data` flags. The worker's payload build reads them, and every
  `llm_req` exit deletes the `hook_directives` rows.
- `postAdvance` fires on the main thread at the worker-completion wake
  (`src/main.c`), before `advance_session` consumes the result.
- `beforeToolCall` fires after capability resolution, before execution.
- `afterToolCall` fires after execution and after the secret scanner, before
  the tool-result entry is written.

```
main thread                         worker thread
  dispatch_llm_req()
    preAdvance → hook_directives
    submit llm_jobs row  ───────▶     llm_req(): build payload (reads
                                        hook_directives), HTTP, write entries,
                                        clear hook_directives
  worker-completion wake  ◀───────
    postAdvance → transform/annotate
  advance_session()
    tool_use: beforeToolCall → exec → secret scan → afterToolCall → write
```

**Timing:** hooks are synchronous and blocking. A slow hook delays the turn.
The shared hooks runtime has a heap limit and an instruction-count interrupt
(`js_runtime_create`, `src/tool_js.c`); there is no per-hook timeout.

**Loading:** `agent_setup_init` calls `extension_load_hooks` once. It loads
the hooks of the agent's attached, enabled extensions that are published or
owned by the agent, in `ORDER BY extension_name, event, path`.

**DB access:** none. Mutations happen only through the returned command
object, which C validates before it applies anything.

---

## 6. QJS Globals Available to Hooks

The hooks context (`QJS_PROFILE_HOOKS`, `src/qjs_helpers.c`) has reduced
intrinsics: base objects, `eval`, `JSON` and `RegExp`. There is no `Promise`
and no `console`.

| Global | Type | Notes |
|--------|------|-------|
| `input` | object | The hook's input JSON (§3); also passed as the function argument |

There is no `db`, `kv`, `fs`, `console` or `http_request`. Hooks run
in-process, not in a sandboxed child, so filesystem and network access stay
tool concerns. A `db.query` global and a per-extension `kv` store are possible
future additions.

---

## 7. Registration

Hooks are declared in the extension manifest:

```json
{
  "hooks": [
    {"event": "preAdvance", "handler": "remind.qjs"},
    {"event": "postAdvance", "handler": "redact_pii.qjs"},
    {"event": "beforeToolCall", "handler": "gate.qjs"}
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

`extension_load_hooks()` reads each handler file into `ExtensionCtx.hooks[]`
(one list per `HookEvent`).

---

## 8. Chaining and Conflict Resolution

| Event | Multiple hooks | Resolution |
|-------|---------------|------------|
| `turnStart` / `turnEnd` | All run (order irrelevant) | — |
| `preAdvance` | Chained in load order | Commands accumulate. Conflicting `suppress` vs `pin` on same ID: `pin` wins. |
| `postAdvance` | Chained in load order | Last `transform_content` wins. `annotate` merges. |
| `beforeToolCall` | Most-restrictive wins | deny > ask > allow |
| `afterToolCall` | Chained in load order | Each sees previous `result` |

---

## 9. Security Considerations

- **No write access to DB from hooks.** All mutations go through the command
  object, validated by C. A hook cannot `INSERT INTO entries` directly.
- **Injection is capped.** `inject` allows at most 3 entries and 2048 chars
  per dispatch (`HOOK_INJECT_MAX_ENTRIES`, `HOOK_INJECT_MAX_CHARS`).
- **`suppress` cannot hide the triggering user message.** Prevents a hook from
  silently dropping the user's instruction.
- **`transform_content` preserves original.** Stored in `data.original_content`
  so redaction is auditable.
- **Hook source is loaded from the shared extension store** (not the draft
  workspace). Only promoted extensions' hooks run.

---

## 10. Examples

Each handler file is one function expression. Handler files must end in `.qjs`.

### Inject a reminder before each LLM call

```javascript
// remind.qjs — preAdvance hook
(function (input) {
    if (input.pending_tool_calls > 0) return {};
    return {
        inject: [{role: "system", content: "Reply in under 200 words.", ephemeral: true}]
    };
})
```

### Redact secrets from assistant output

```javascript
// redact_pii.qjs — postAdvance hook
(function (input) {
    var patterns = [/sk-[a-zA-Z0-9]{20,}/g, /ghp_[a-zA-Z0-9]{36}/g, /AKIA[A-Z0-9]{16}/g];
    var content = input.content || "";
    var out = content;
    for (var i = 0; i < patterns.length; i++) out = out.replace(patterns[i], "[REDACTED]");
    if (out === content) return {};
    return {transform_content: out, annotate: {redacted: true}};
})
```

### Block a tool for review

```javascript
// gate.qjs — beforeToolCall hook
(function (input) {
    if (input.name === "shell_exec" && /rm -rf/.test(JSON.stringify(input.args)))
        return {ask: true, reason: "destructive shell command"};
    return {};
})
```

---

## 11. Open Questions

- **Should `preAdvance` see the context plan metadata** (which entries are
  planned for inclusion, estimated tokens per entry)? Useful for smart
  suppression but exposes internal context-window logic to hooks.
- **`notify` routing** (if `notify` is built) — through the channel outbox
  table, or a lighter sideband? The outbox requires a session_id; hook
  notifications are session-scoped, so the outbox works.
- **Hook testing CLI** — `cclaw --test-hook <event> <handler.qjs> [input.json]`
  to dry-run a hook against synthetic input without running a real turn.
