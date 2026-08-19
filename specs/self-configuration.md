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
| **Write (single)** | agent → config | `request_config` action `request_changes` — one JSON document batching grants, config values, and/or a provider definition; `rename_agent` | human approval (park → apply, all-or-nothing) |
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

## `request_changes`

`request_config` action `request_changes` takes a single `changes` JSON document — one human approval covers the whole batch. The document uses the **same grants dialect** as `extension.json`'s `$.agents[].grants` — two routes, one shape: extension manifests declare new named things; `request_changes` mutates the calling agent's own live state.

```json
{"action":"request_changes","changes":{
   "grants":{"tools":["name"],"hosts":[".example.com"],"read_paths":["/abs"],"write_paths":["/abs"]},
   "agent":{"models":["gemini-2.5-flash@gemini"],"max_iterations":40},
   "routes":["telegram:12345"],
   "config":{"registered.key":"value-string"},
   "provider":{"provider":"openrouter","base_url":"https://openrouter.ai/api/v1"},
   "models":[{"id":"deepseek/deepseek-v4-flash@openrouter","context_window":128000}]
 },"reason":"shown to approver"}
```

Sections by scope — the approval prompt groups them the same way:

| Section | Scope | Applies as |
|---------|-------|-----------|
| `grants` | agent | `grants` rows for the caller |
| `agent` | agent | the caller's own settings — `models` is a whole-list replace of the caller's `agent_models` routing order (first entry = primary); `max_iterations`/`shell_timeout` are columns on its `agents` row |
| `routes` | agent | session + `channel_routes` pin (`channel:chat_id`, first-come, `explicit` delivery; no wildcards — channel defaults are operator config) |
| `config` | **system** | global `config` table |
| `provider` | **system** | `providers` upsert — transport only (endpoint + credential name) |
| `models` | **system** | `models` upsert per entry — register, update, or disable by canonical id |

- **Models are separate from providers.** A provider is protocol/endpoint/auth,
  set once and near-fixed; models change often and carry the things the loop
  actually reads (context window, capabilities). Registration
  therefore points the same way the runtime does: `models` entries name a
  provider that must **already** exist (or be defined by the same document),
  and `provider` documents no longer carry a `model` key at all.
  `providers.default_model` survives as fresh-install seed sugar
  (`templates/seed.sql`) and is not a registration path.
- **Model ids are canonical.** `agent.models` and
  `create_agent`/`update_agent` accept `models.id` entries and nothing else; a bare
  name gets a did-you-mean naming the registered id. Bare names used to resolve
  to whichever provider's row the scan reached first — schema v44 rewrote the
  existing ones once (unambiguous → rewritten, no match → left as-is,
  ambiguous → the row routing preferred), and no fallback parser was kept.
- **Eager validation (typo-hostile)**: unknown sections/keys are errors, never
  dropped. Config keys must be registered (`search_config` lists them);
  secret-flagged keys are rejected (`save_secret` is the path for those).
  Provider defaults are filled at park time so the approver sees the final values;
  an existing provider's `api_key_env` is **preserved verbatim, empty included**
  (an absent field means "leave it", never "re-derive it" — the derivation
  applies to genuinely new providers only). A document whose credential name
  resolves to neither an environment variable nor a system-scope secret is
  refused at request time, where the agent can still fix it, rather than
  silently skipped by routing later.
  Paths must be absolute. Routes already owned by another agent are
  refused. A document that can't apply never parks.
- **All-or-nothing apply**: on approval, the entire document is applied inside a
  savepoint. If any line fails (including a route captured between park and
  apply), the whole document rolls back.
- **Verify-after-write receipt**: the success notice is built by **re-reading**
  the rows each section claims to have written (live grants, stored config
  values, the provider row, each model row, the agent's own settings, bound
  routes) — never by echoing the request. Intent and effect diverged silently
  in the 2026-08-10 incident, and the agent has no other way to tell.
- **Probe at apply (routing changes only)**: a re-read proves what the rows
  *say*, not that a request now reaches a model — the 2026-08-10 incident wrote
  a perfectly valid provider row and knocked prod off its gateway for 2.5h. So
  a document that changes **which model would serve the next request** (the
  agent's own routing list, a `models` entry on that list, or the provider
  behind one of them) is verified for real before it stands:
  the rows it is about to move are snapshotted, the write commits, and then the
  new top candidate gets **one minimal completion** (`max_tokens` 1) over the
  normal transport — same candidate loader, same URL/auth builders, **15s hard
  timeout, no retries, no fallback ladder**; a timeout is a failed probe. The
  call is synchronous in the approval path, and the resulting rare ~15s stall
  is accepted deliberately (the alternative was a pending-probe approval
  state). Success appends `probed OK — <id> served the test request`, naming
  the id the *provider* reported. Any failure restores the snapshot (rows the
  document created are deleted, rows it changed are put back) and the tool
  result is exactly `probe failed: <reason> — reverted to <previous>`. Routing
  is never silently rerouted. The probe writes nothing but an `llm_responses`
  archive row (`probe_ok`, `probe_http_<code>`, `probe_timeout`,
  `probe_network_error`) so `cclaw resp` can show it — no entries, no turn
  state, no model stats or degradation bookkeeping. Grants/config-value-only
  documents are not probed, and neither are the operator paths (dashboard, CLI
  grant-from-history): they have no session and no tool result to read the
  verdict, and pre-request candidate drops already surface there (see
  [error-handling.md](error-handling.md)).
- **Approval summary**: enumerates every requested line (hosts, paths, tools,
  routes, agent k=v, config k=v, models, provider) in fenced blocks grouped
  **agent-scoped vs system-wide** so the approver sees the blast radius at a
  glance — values, never counts. A provider document aimed at an **existing**
  row renders a field-level diff (`provider X base_url: old -> new`), not the
  canonical doc: re-stating fields that did not move is how a single rewritten
  `api_key_env` went unnoticed.

### Scoping model (why `config` is system-wide)

Agent-level state lives on agent-keyed tables (`agents` columns, `grants`,
`agent_extensions`, `channel_routes`, `memory_blocks`) and is mutated by the
agent-scoped sections above. The `config` registry is deliberately global —
one home for daemon-wide knobs, not a per-agent namespace. If a knob needs
per-agent values, it becomes an `agents` column (whitelisted into the `agent`
section), not a scoped config key.

### Attachment model (one-liner each)

- **provider → model → agent**: a provider is system-level transport; a model
  is registered *on* a provider (`models` section, canonical `model@provider`
  id); an agent *adopts* models by declaring `agent.models` — its full
  replacement routing order.
  Providers are only reachable through `models` rows (per-request routing joins
  models → providers), so a provider with no models row is unreachable — one
  document can carry all three steps.
- **session → agent**: fixed at creation (`launch_agent`, channel routing);
  never reassigned. Moving work = a new session, not a transfer.
- **session → channel**: a session's `channel_name`/`chat_id` is its
  origin, set at creation or attached by the operator — not agent-mutable.
  What an agent requests is a **route** (send authority + inbound routing for
  a chat), not a rebinding of an existing session.

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
  "models": ["…", "…"],
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

Creation stamps `agents.created_by` (NULL = operator) — the provenance that
authorizes later updates.

**Applied by one function**: `agent_definition_apply(db, json, creator, err)`
(src/agent_define.c). All three declaration paths funnel through it, so caps,
validation, and side effects (agents row, grants rows, `agent_extensions`,
memory-block seed, workspace dir) cannot diverge. The DB is the only home —
nothing writes `agent.json` back to disk; the disk file is a one-shot seed.

### Creation caps (trust.md)

When `creator` is non-NULL (agent-initiated: `create_agent`, or a manifest
promoted by an agent), the child is capped on both axes independently:

- **Containment**: child `sandbox_profile` ≤ creator's, by looseness order
  `host > standard > restricted`. Requesting a looser profile is a
  hard refusal, not a clamp — the definition was wrong, say so.
- **Authority**: child grants ⊆ creator's *live* grants (exact
  subset-of-values per kind: tool, host, read_path, write_path). No fuzzy
  comparison. Grants the creator lacks are refused by name.
- **Extensions**: each named extension must be published, or owned by the
  creator.

`creator == NULL` means operator (disk seed, CLI): no caps.

Approval is still required on top of the caps: `create_agent` parks its own
apply-approval like `request_config` (it is deliberately *not* in
`agent_approval_tools` — a standing approval_mode would double-prompt), and
the approval summary enumerates what is being created: profile, N tools,
M hosts, extensions, whether cloned. Eager validation happens at request
time — a definition that would fail caps or parse never parks.

### Updating an existing agent

`update_agent` (src/tool_bootstrap.c → `agent_definition_update_*`,
src/agent_define.c) revises an agent after creation — the "refine it later"
path that create-then-tune workflows need, since a definition is otherwise
immutable to agents once applied.

- **Authorization**: the caller must be the target itself or its creator
  (`agents.created_by`). Pre-v30 agents have NULL `created_by` — operator-only,
  the conservative reading of unknown provenance.
- **Same caps as creation, against the caller**: `sandbox_profile` never
  looser than the caller's; every added grant ⊆ the caller's live grants.
  Grants are **additive-only** — removal is an operator act. A self-update's
  grant adds are no-ops by construction, so authority *expansion* remains
  exclusively `request_changes` (escalation) and the delegation/escalation
  split survives.
- **Overlay semantics, typo-hostile**: provided fields overwrite
  (`description`, `system_prompt`, models — canonical `models.id` only —
  `sandbox_profile`, `max_iterations`, `shell_timeout`); absent fields keep
  their value, so unknown keys are hard errors (a typo would otherwise
  silently change nothing). An update with no updatable field is an error.
- **Approval**: parks like `create_agent`; the summary enumerates every
  changed field (long values clipped per line) and every added grant.
  Validation re-runs at apply — authorization or caps that shifted between
  park and approval fail the apply.

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

An `agents[]` name that already exists is **skipped, never overwritten** —
first-come, mirroring extension-name ownership, and it keeps re-promoting the
same bundle idempotent (its own agents exist by then).

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
existing primitives (`extension_attach` + `request_changes` config section + secrets).

## Docs: self-knowledge as skills

Agents learn what they can do the same way they learn anything else:
**skills**, with the index giving one-line progressive disclosure and the
body loaded on demand. Self-docs ship as a builtin, skills-only extension
`cclaw-docs` (owner `system`), installed at daemon start like the telegram
builtin, containing operational how-tos distilled from specs/:

| Skill | Covers |
|-------|--------|
| `configuring-cclaw` | config registry, search_config, request_config request_changes document (scoped sections), env precedence |
| `extending-cclaw` | manifest format, draft→promote→publish→attach, handler contracts, JS dialect limits |
| `creating-agents` | agent-definition schema, creation caps, clone_from, the escalate-then-delegate two-step, manifest `agents[]` packaging — top-level because agent creation is a headline capability |
| `cclaw-agents` | roster, launch_agent / check_session delegation, session↔agent↔channel binding, when to *offer* creating an agent |
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

All five phases are implemented:

1. `request_changes` action (`src/tool_request_config.c`).
2. `agent_definition_apply` (`src/agent_define.c`) + real `create_agent`;
   `configure_channel` deleted.
3. `agents[]` manifest component + promote enumeration
   (`src/extension_manifest.c`, `src/tool_extension.c`).
4. config.md implementation (env layer, secret keys, launch gate).
5. `cclaw-docs` builtin skills extension (`templates/docs_*.md`, installed by
   `extension_install_builtin`) + search_config extension/roster sections.
