---
name: cclaw-secrets-memory
description: Using secrets safely ({{SECRET:name}} interpolation, save_secret, secret_create, bindings) and how memory blocks work — labels, placement, and editing.
---

# Secrets and Memory

## Secrets

Secrets are stored encrypted (ChaCha20-Poly1305) in the database. You never
see raw values: a DLP scanner redacts credential-shaped strings from tool
results before they reach you, and secret config keys never resolve into your
context.

**Using a secret**: write the placeholder `{{SECRET:NAME}}` in tool arguments
(`shell_exec`, `web_fetch`, `js_eval`). CClaw interpolates the real value just
before execution — the context never contains it. Example:

```
curl -H "Authorization: Bearer {{SECRET:GITHUB_TOKEN}}" https://api.github.com/user
```

**Capturing a secret** (explicit, never automatic):

- `save_secret` is an *argument*, not a tool: when a call will RETURN a
  credential (minting a token, reading a key), add `save_secret: "NAME"` —
  plus `save_secret_path: "$.field"` if the result is JSON — to that same
  call. The value is stored encrypted without entering your context.
- `secret_create {name}` — mint a new random credential (stored encrypted;
  you get only the `{{SECRET:name}}` placeholder back). Not a baseline grant:
  the default agent has it, others request it.
- The operator can store one directly with `cclaw secret set NAME` or through
  the environment. Ask them to; never ask for the value in chat.

Secrets have scopes (`system` vs agent) and host bindings — a secret may only
be submitted to hosts it is bound to. A call using an unbound secret (or
aiming at an uncovered host) is denied with the missing pair named: request it
with `request_config` (`secret_bindings: {"NAME": ["host"]}`), batching every
host a denial named into one document, then re-issue the call. If a tool
result shows `[REDACTED:…]`, the scanner caught a credential; use
`save_secret` if it should be kept, and never try to reconstruct it.

## Memory blocks

Memory is **blocks** holding **notes**, rendered into your context. A block
is a named container with a `label` and a `description` (when/why to use it);
a note is one item of text in it, shown as `[id date] text`. The id is how
you address a note; it never changes and is never reused, so deleting one
note does not renumber the others. The date is when the note last changed —
an old date is a reason to check the note is still true.

Each block's header reads
`usage: <used>/<limit> chars | read-write | placement: context`:

- `placement: context` — shown in the per-turn context block, refreshed at the
  start of every turn. Right for notes you edit.
- `placement: system` — part of the system prompt, rendered once per session;
  an edit shows from your next session. Only for stable identity content.
- `read-only` — you can see the block but not change it.
- `shared` — another agent owns the block and you see the same notes.

- Use blocks for durable facts: who the operator is, standing preferences,
  project state that must survive sessions.
- Keep them short and current — they cost context every turn. Prune rather
  than append: a note is a fact that changes what you do next time, not a
  record of what happened. Correct a note that has gone out of date (edit
  it) instead of adding a second one beside it.
- A block has a size limit. When an add or edit is refused because the block
  is full, delete or merge notes first.
- Session history is not memory: anything you want to survive compaction or a
  new session must go into a block.

Tools: `memory_block_create` (label, description, placement) and
`memory_block_update` (description, placement) for blocks;
`memory_note_add` (block, text), `memory_note_edit` (block, notes:
[{id, text}]) and `memory_note_delete` (block, ids) for notes. Agent
definitions and extension manifests can seed initial blocks via
`memory_blocks[]`; `{"attach": <block id>}` there shares one of your own
blocks with the new agent (read-only unless `read_only` is false).
