---
name: configuring-cclaw
description: How to read and change CClaw configuration — search_config, request_config (the changes document), env-var precedence, and the config registry.
---

# Configuring CClaw

CClaw's runtime configuration lives in a single registry (the `config` table plus
built-in defaults). You interact with it through two tools.

## Reading configuration

`search_config` returns your current view: registered config keys with resolved
values, your grants (tools, hosts, paths), attached extensions, and the agent
roster. Call it with a query string to filter, or empty to see everything.

Resolution order for every key (highest wins):

1. Environment variable `CCLAW_<KEY>` — uppercase, `.` becomes `_`
   (e.g. `telegram.bot_token` → `CCLAW_TELEGRAM_BOT_TOKEN`). Read live, never
   stored.
2. The stored value (`config.value`), set by the operator or an approved
   config change.
3. The registered default.

Keys flagged **secret** never resolve from the stored value: they resolve from
the environment or the encrypted secrets store only, and config writes refuse
them (ask the operator to use `save_secret` / the environment instead).

## Changing configuration

All writes go through `request_config`, which parks an approval for the
operator. Batch everything one task needs into ONE changes document —
one approval covers the whole document:

```json
{"changes": {
   "grants": {"tools": ["shell_exec"], "hosts": [".example.com"],
              "read_paths": ["/abs/path"], "write_paths": ["/abs/path"],
              "remove": {"hosts": ["old.example.com"]}},
   "agent": {"models": ["gemini-2.5-flash@gemini"], "max_iterations": 40},
   "routes": ["telegram:12345"],
   "config": {"registered.key": "value"},
   "provider": {"provider": "gemini"},
   "models": [{"id": "gemini-2.5-flash@gemini", "context_window": 1000000}]
 }, "reason": "why you need these"}
```

- Any subset of the sections works. Validation is all-or-nothing and strict:
  unknown sections or keys are errors, never silently dropped.

**Agent-scoped sections** (change only you):

- `grants`: host prefix `.` covers subdomains (`.example.com` covers
  `example.com` AND `api.example.com`); shell/web egress is default-deny.
  Paths must be absolute; `read_paths` and `write_paths` are separate.
- `agent`: your own settings — `models` (your FULL replacement routing order:
  an array of canonical model ids exactly as `search_config` lists them, or
  ids this same document's `models` section registers; first entry is primary;
  bare model names are refused with the id you probably meant),
  `max_iterations`, `shell_timeout`.
- `routes`: `"channel:chat_id"` strings authorizing `channel_send` to that
  chat. First-come: a chat routed to another agent is refused. Wildcards are
  operator-only.

**System-wide sections** (change the whole daemon — expect more scrutiny):

- `config`: keys must exist in the registry; unknown keys fail immediately
  (no approval is parked), so check `search_config` first. Values are strings.
- `provider`: define an LLM provider's **transport** — `provider`, `base_url`
  for custom names, and `api_key_env` naming the secret that holds the API key.
  Store the key first with `save_secret` (never pass key material): a provider
  whose key does not exist is refused here, because at request time its models
  are skipped in silence. A keyless endpoint says so explicitly with
  `"api_key_env": ""`. Editing an existing provider leaves its credential name
  alone unless you pass a new one.
- `models`: register, update, or disable a model — `[{"id": "<model>@<provider>",
  "context_window": N, "max_output_tokens": N, "capabilities": ["text"],
  "status": "healthy"|"disabled"}]`. The provider must already
  exist (or be defined by this same document); only `id` is required, and
  fields you omit keep their current values. One document can therefore define
  a provider, register a model on it, and adopt it via
  `agent.models: ["<model>@<provider>", ...]`.

The document is a PATCH against what `search_config` shows: `agent.*` fields
replace, `grants.*` add, `models`/`provider` upsert, and
`grants.remove` (same four kinds) gives up grants you already hold — applied
immediately, with no approval.

Give a concrete `reason` — the operator sees it in the approval prompt. If the
approval is denied, do not re-request the same thing; explain what you were
trying to do and let the operator decide.

## Channel configuration

Channels are configured through namespaced registry keys owned by their
extension: `<extension>.<key>` (e.g. `telegram.enabled`, `telegram.admin_ids`).
A channel launches only when its trust status is active, `<ext>.enabled` is
truthy, and every key flagged `required` resolves to a non-empty value.
Enabling a channel is therefore a one-key `config` change on `<ext>.enabled`
(plus any missing required keys).
