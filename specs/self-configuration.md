# Self-Configuration

How an agent inspects, changes, and extends its own configuration — and how
those powers stay inside the trust model. This spec defines the four
self-config surfaces, the **agent-definition schema** (one JSON shape used
everywhere an agent is declared), the trust rules that cap what a
self-created agent may be, and how self-knowledge (docs) is distributed.

Companion specs: [trust.md](trust.md) (axis model), [config.md](config.md)
(resolution rule), [extensions.md](extensions.md) (manifest lifecycle),
[security.md](security.md).

---

## The four surfaces

| Surface | Direction | Mechanism | Gate |
|---------|-----------|-----------|------|
| **Read** | agent → config | `search_config` (curated view), `db_query` (arbitrary SELECT) | tool grant |
| **Write (single)** | agent → config | `request_config` actions: `grant_tool`, `grant_host`, `grant_path`, `rename_agent`, `set_config` | human approval (park → apply) |
| **Write (bulk/structured)** | agent → system | extension manifests via `extension_promote` — tools, hooks, channels, scripts, skills, config keys, **agents** | human approval with enumerated contents |
| **Docs** | system → agent | builtin `cclaw-docs` skills extension (progressive disclosure via the skills index) | none (read-only knowledge) |

Design invariants (from the architecture review, SELF-CONFIGURATION-REVIEW.md):

- **Tools are the only agent-initiated write path.** Every change is a tool
  call in `entries` — audited, approvable, transactional. No file-based
  config alternative, no separate mutation API.
- **`db_query` is the read path.** No per-surface read tools.
- **One home for state**: the config registry (`config` table + C registry +
  extension-registered keys). Approval is the existing park/apply machinery,
  not a config-specific system.

## `set_config`

`request_config` gains action `set_config {key, value, reason?}`:

- **Request-time validation**: the key must exist — in the C registry
  (`config_default()`) or as an extension-registered row (a `config` row
  with a code-owned `default_value`). Unknown keys fail immediately with an
  error naming `search_config` as the way to discover keys. Secret-flagged
  keys are rejected (`config_set` refuses them; see config.md).
- **Park/apply**: same gate as the grant actions. The approval summary shows
  `set_config: <key> = <value>`.
- **Apply**: `apply_grant()` calls `config_set()`; failure surfaces as a
  tool error result.

## Agent-definition schema

One JSON object describes an agent, everywhere an agent is declared:

1. `agents/<name>/agent.json` on disk — the **seed** path (`db_agent_seed`),
   operator-authored.
2. `create_agent` tool arguments — the **self-apply** path, approval-gated.
3. `extension.json` `agents[]` — the **manifest** path, gated by promote
   approval.

```json
{
  "name": "Watcher",
  "description": "Monitors feeds and reports anomalies",
  "system_prompt": "…",
  "primary_model": "…",
  "secondary_model": "…",
  "sandbox_profile": "standard",
  "grants": {
    "tools": ["web_fetch", "file_read"],
    "hosts": [".example.com"],
    "read_paths": ["/data/feeds"],
    "write_paths": []
  },
  "extensions": ["nws"],
  "memory_blocks": [
    { "label": "persona", "description": "…", "value": "…",
      "char_limit": 5000, "read_only": false, "placement": "system" }
  ],
  "max_iterations": 25,
  "shell_timeout": 30,
  "clone_from": "Assistant"
}
```

Only `name` is required. Everything else defaults: profile from the operator
default, grants from `agent_default_tools` (+ `agent_approval_tools` modes),
no extensions beyond `agent_default_extensions` (see Docs below), no memory
blocks. In a manifest, `system_prompt_file` (bundle-relative path) may replace
`system_prompt`.

`clone_from` copies an existing agent's row, grants, and extension
attachments first; the rest of the definition overlays. The clone source must
exist.

**Applied by one function**: `agent_definition_apply(db, json, creator, err)`
(src/agent_define.c). All three declaration paths funnel through it, so caps,
validation, and side effects (agents row, grants rows, `agent_extensions`,
memory-block seed, workspace dir) cannot diverge. The DB is the only home —
nothing writes `agent.json` back to disk; the disk file is a one-shot seed.

### Creation caps (trust.md)

When `creator` is non-NULL (agent-initiated: `create_agent`, or a manifest
promoted by an agent), the child is capped on both axes independently:

- **Containment**: child `sandbox_profile` ≤ creator's, by looseness order
  `host > trusted > standard > restricted`. Requesting a looser profile is a
  hard refusal, not a clamp — the definition was wrong, say so.
- **Authority**: child grants ⊆ creator's *live* grants (exact
  subset-of-values per kind: tool, host, read_path, write_path). No fuzzy
  comparison. Grants the creator lacks are refused by name.
- **Extensions**: each named extension must be published, or owned by the
  creator.

`creator == NULL` means operator (disk seed, CLI): no caps.

Approval is still required on top of the caps: `create_agent` sits in
`agent_approval_tools` (approval_mode `always`), parks like `request_config`,
and the approval summary enumerates what is being created: profile, N tools,
M hosts, extensions, whether cloned. Eager validation happens at request
time — a definition that would fail caps or parse never parks.

### Uninstall / deletion

Uninstalling an extension that declared agents does **not** delete the
agents — they own sessions, memory, and a workspace. Agent removal is an
operator verb (future `cclaw agent rm`), never a side effect.

## Manifests declare agents

`extension.json` gains `agents[]`: an array of agent-definition objects
(`system_prompt_file` allowed). Validation (`extension_manifest_validate`)
checks name shape, profile value, and grants shape; caps are enforced at
install time against the promoting agent (`creator = owner_agent`; builtin
bundles install as `system` = operator, uncapped).

Name collision with an existing agent refuses the whole install —
first-come, mirroring extension-name ownership.

### Promote approval enumeration

`extension_promote` parks (it is already in `agent_approval_tools`); the
approval prompt now enumerates the validated manifest's declared contents:

> promote 'nws': adds 2 tools, 1 hook, 1 skill, 3 config keys, 1 agent
> (Watcher, standard)

so the approver sees what the bundle will do to the system before code is
registered. The enumeration is generated by querying the manifest JSON before
install (JSON1 over the validated file), not by diffing the DB after.

## Config resolution

[config.md](config.md) is part of this effort (phase 4): the uniform
`env(CCLAW_<KEY>) ?? value ?? default` rule, `secret`/`required` flags on
config keys, channel activation as config, and the builtin telegram bundle's
real manifest. Once landed, "configure a channel" = attach/enable the
extension + set its config keys — which is why the `configure_channel` tool
is deleted rather than implemented: it was a stub whose job decomposed into
existing primitives (`extension_attach` + `set_config` + secrets).

## Docs: self-knowledge as skills

Agents learn what they can do the same way they learn anything else:
**skills**, with the index giving one-line progressive disclosure and the
body loaded on demand. Self-docs ship as a builtin, skills-only extension
`cclaw-docs` (owner `system`), installed at daemon start like the telegram
builtin, containing operational how-tos distilled from specs/:

| Skill | Covers |
|-------|--------|
| `configuring-cclaw` | config registry, search_config, request_config actions incl. set_config, env precedence |
| `extending-cclaw` | manifest format, draft→promote→publish→attach, handler contracts, JS dialect limits |
| `cclaw-agents` | agent-definition schema, create_agent / launch_agent / check_session, grants model, when to *offer* creating an agent |
| `cclaw-channels` | channel component, activation via config, outbox/inbox flow |
| `cclaw-secrets-memory` | `{{SECRET:}}`, save_secret / secret_create, bindings; memory blocks and placement |

Distribution: registry key `agent_default_extensions` (default
`["cclaw-docs"]`) is attached by `agent_definition_apply` at creation; the
builtin install backfills `INSERT OR IGNORE INTO agent_extensions` for
existing agents. Detachable per-agent like any extension — docs are a
default, not a mandate.

`search_config` additionally surfaces two navigation sections: **Your
extensions** (attached + enabled) and **Agent roster** (name + description)
— the inputs to "should I delegate, attach, or propose a new agent?".

## Implementation status

Phased; each phase lands complete with tests:

1. `set_config` action — implemented with this spec.
2. `agent_definition_apply` + real `create_agent`; `configure_channel`
   deleted.
3. `agents[]` manifest component + promote enumeration.
4. config.md implementation.
5. `cclaw-docs` builtin + navigation sections.
