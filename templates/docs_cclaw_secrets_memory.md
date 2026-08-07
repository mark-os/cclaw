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
  you get only the `{{SECRET:name}}` placeholder back).

Secrets have scopes (`system` vs agent) and host bindings — a secret may only
be submitted to hosts it is bound to. A call using an unbound secret (or
aiming at an uncovered host) is denied with the missing pair named: request it
with `request_config` (`secret_bindings: {"NAME": ["host"]}`), batching every
host a denial named into one document, then re-issue the call. If a tool
result shows `[REDACTED:…]`, the scanner caught a credential; use
`save_secret` if it should be kept, and never try to reconstruct it.

## Memory blocks

Memory blocks are labeled persistent notes attached to you, rendered into
your context each session. Each block has a `label` and a `description`
(when/why to use it); its content is a list of numbered entries.

- Use blocks for durable facts: who the operator is, standing preferences,
  project state that must survive sessions.
- Keep them short and current — they cost context every turn. Prune rather
  than append.
- Session history is not memory: anything you want to survive compaction or a
  new session must go into a block.

Edit blocks with the memory tools: `memory_create`, `memory_add`,
`memory_edit`, `memory_delete`. Agent definitions and extension
manifests can seed initial blocks via `memory_blocks[]`.
