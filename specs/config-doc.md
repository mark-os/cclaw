# The config document — one schema, read and written

Status: M2 shipped. Design history in `plan/projects/config-doc.md`.

CClaw has **one** vocabulary for an agent's configuration. `search_config`
renders it (prose, or `format:json` — the canonical document), `request_config`
patches it, and the approval card shows a human what a patch would change.
Same section names, same shapes, three surfaces. The rule that keeps them
honest: **read-shape ≈ write-shape**. If a section appears in what
`search_config` shows you, that is the section name you patch.

Four consumers interpret these names; a change to one is a change to all:

| Consumer | Where |
|----------|-------|
| park / validate | `src/tool_request_config.c` (`validate_changes`) |
| apply | `request_config_changes_apply` (same file), driven by `apply_grant` in `src/resolve.c` |
| render (approval card) | `src/approval.c` (`SQL_CHANGES_*`) |
| export (`format:json`) | `src/tool_search_config.c` (`SQL_CONFIG_DOC`) |

## Sections

`request_config` takes `{changes, reason}` — no action field, no second mode.
`changes` is an object; every key is optional, an unknown key is an error
(typo-hostile by design — a silently dropped section is a config change that
never happened and a receipt that says it did).

| Section | Shape | Scope | Kind |
|---------|-------|-------|------|
| `agent` | `{models?: [id], max_iterations?: n, shell_timeout?: n}` | this agent | settings |
| `grants` | `{tools?, hosts?, read_paths?, write_paths?: [value], remove?: {…same kinds}}` | this agent | authority |
| `routes` | `["channel:chat_id"]` | this agent | authority |
| `secret_bindings` | `{SECRET_NAME: [host]}` | this agent | authority |
| `config` | `{key: "value"}` | system-wide | settings |
| `provider` | `{provider, base_url?, api_key_env?}` | system-wide | registry |
| `models` | `[{id: "model@provider", context_window?, max_output_tokens?, capabilities?, status?}]` | system-wide | registry |

Two axes, both shown on the approval card:

- **Scope** — whose reach changes. `agent`/`grants`/`routes`/`secret_bindings`
  touch only the asking agent. `config`/`provider`/`models` are system-wide:
  every agent sees them.
- **Kind** — *authority* widens what the agent can reach (the line an approver
  must never skim past); *settings* retune what it already has; *registry*
  records what exists (a provider endpoint, a model's context window) without
  by itself pointing any agent at it.

`sandbox_profile` is **containment, not authority**, and is on no axis here:
no `changes` section can move it. Widening what an agent can see is always
`grants` plus an approval (see [trust.md](trust.md),
[sandbox-profiles.md](sandbox-profiles.md)).

## Patch semantics — the verb table

The changes document is a **patch against the state `search_config` shows**,
never a whole-world replacement. The verb differs per section, and the tool
description states it in one line so the model does not have to guess:

| Section | Verb | Meaning |
|---------|------|---------|
| `agent.*` | **replace** | Each named field replaces its current value. `agent.models` is the *whole* routing list, in order, first = primary; unnamed fields keep their values. |
| `grants.*` | **add** | Values are added to what the agent already holds. Values it already has are filtered out before parking, so an approver only decides what is new. |
| `grants.remove` | **remove, immediately** | See below. |
| `routes` | **add** | First-come; a route owned by another agent is refused. |
| `secret_bindings` | **add** | (secret, host) pairs are added; existing pairs are filtered out. |
| `config` | **replace** | Per key. Keys must already be registered. |
| `provider` | **upsert** | Insert, or update `base_url`/`api_key_env` in place. Transport only — never key material, only the secret's *name*. |
| `models` | **upsert** | Insert, or update the fields the entry actually carries. Absent fields keep their stored values. `status:"disabled"` retires a model. |

Everything above except `grants.remove` **parks for human approval** and applies
all-or-nothing under one savepoint. One document, one approval — batching
related needs (define a provider, register a model on it, adopt it via
`agent.models`) is the intended usage, not an abuse of it.

## `grants.remove` — narrowing never waits

```json
{"changes": {"grants": {"remove": {
  "hosts": ["api.example.com"], "tools": ["shell_exec"],
  "read_paths": ["/srv/data"], "write_paths": ["/srv/out"]}}}}
```

Applied **immediately, in the handler call, with no approval**. Widening parks;
narrowing never does. An agent giving up authority it already holds needs
nobody's permission, and putting the safest possible request behind the slowest
possible path would teach exactly the wrong lesson.

Rules:

- Only the four grant kinds. An unrecognized kind is refused by name. The path
  only ever `DELETE`s from `grants`, so `sandbox_profile` and every system-wide
  section are out of its reach by construction, not by check.
- Every value must be a **live grant of this agent**. Removing something you
  never had is a mistaken belief about your own state, not a no-op — it is
  refused with the offending value named, and nothing is removed.
- The receipt is a **re-read**: it lists what the `grants` table no longer has,
  not what the request asked for.
- Mixing `remove` with widening sections is legal: the removals apply at once
  and the remainder parks as usual. Because a parked call's result reaches the
  model only at the decision, the removal receipt rides the approver-facing
  `reason` instead of being dropped.

## The receipt contract

Receipts over intent. The 2026-08-10 and 2026-08-19 incidents were both the
same failure: an agent asserting a config change had taken effect when it had
not, because nothing in the loop ever re-read the state.

- **Applied.** Every line of the apply receipt is a re-read of the row the
  section claims to have written (`RECEIPT_SQL`, `tool_request_config.c`) —
  never an echo of the request. Documents that move routing are additionally
  probed for real; a failed probe reverts to the pre-apply snapshot and the
  failure *is* the receipt.
- **Denied.** Carries provenance in plain language — who decided and via what
  channel (`approval_decider_phrase`) — states that this is a decision and not
  an error, and ends with a restatement of current effective state
  (`approval_state_restatement`). A denial stands: re-parking the same
  canonical document in the same session is refused inline.
- **Expired.** Still explicitly *not* a denial (re-requesting is invited), but
  carries `Nothing was applied` plus the same state restatement.
- **Every terminal state restates effective status.** No terminal message
  leaves the model free to confabulate what its config now is.

`search_config` closes the loop from the other side: a **pending approvals**
section in both formats (id, age, one-line summary — `approval_format_summary`,
clipped in the prose view), pending-only, matching the `<open_approvals>`
context block. "Waiting on a human" is the one approval state the model can
act on.

## `format:json`

`search_config {"format":"json"}` returns the canonical document: the same data
as the prose view, one query layer and two renderers so they cannot drift.
Assembled by SQL (`json_object`/`json_group_array` — a query *is* the
serializer, as in `llm_payload.c`), never by C string assembly.

Read-shape sections mirror the write-shape names, with three read-only
additions and one deliberate difference:

- `agent` also carries `name`, `sandbox_profile`, and the effective
  `shell_path` — state you can read but not patch here.
- `session` (`id`, `tool_filter`) — per-session narrowing, not agent config.
  Effective tools = grants ∩ filter.
- `sensitive_hosts`, `tools`, `extensions`, `agents`, `pending_approvals` —
  read-only surfaces with no write counterpart.
- **`providers` is plural** where the write section is the singular `provider`:
  reading shows the whole registry, writing upserts exactly one row.

`config`, `models`, and `tools` honor the optional `query` substring filter,
exactly as the prose view does.

## Future: agent bundles

The document schema above is deliberately the **agent-document fragment** a
future export/import bundle will use, so bundles are a packaging job rather
than a second schema. A bundle is `agent.json` (these sections) plus the
agent's `.qjs` files. Not built — this section is the contract it must honor.

Trust rules for that day, all of them consequences of the model already here:

- **Secrets travel as names only.** A bundle carries `secret_bindings`
  (SECRET_NAME → hosts) and `provider.api_key_env` — never key material. The
  importer must mint or supply its own values via `save_secret`; an import that
  could carry a credential would make every shared agent a credential leak.
- **Foreign grants are requests, not facts.** `grants` in an imported bundle
  parks for approval on the importing host like any other widening. A bundle
  that granted itself hosts and paths on import would be a self-signing
  authority document.
- **Extensions re-enter via draft → promote.** Bundled `.qjs` lands as a draft
  in the importing agent's workspace; registration is the trust boundary and
  stays a separate, approved step. Import must never be a promotion.
- **Registry sections are system-wide on arrival.** `provider`/`models` in a
  bundle affect every agent on the importing host, so they carry the
  system-wide label on the approval card exactly as they do today.
