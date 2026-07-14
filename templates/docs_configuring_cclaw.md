---
name: configuring-cclaw
description: How to read and change CClaw configuration — search_config, request_config (request_changes document, rename), env-var precedence, and the config registry.
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
operator. Batch everything one task needs into ONE `request_changes` document —
one approval covers the whole document:

```json
{"action": "request_changes", "changes": {
   "grants": {"tools": ["shell_exec"], "hosts": [".example.com"],
              "read_paths": ["/abs/path"], "write_paths": ["/abs/path"]},
   "config": {"registered.key": "value"}
 }, "reason": "why you need these"}
```

- Any subset of the sections works. Validation is all-or-nothing and strict:
  unknown sections or keys are errors, never silently dropped.
- `config` keys must exist in the registry; unknown keys fail immediately (no
  approval is parked), so check `search_config` first. Values are strings.
- Host grants: prefix `.` to cover subdomains (`.example.com` covers
  `example.com` AND `api.example.com`). Shell/web egress is default-deny.
- Path grants must be absolute; `read_paths` and `write_paths` are separate.
- A `provider` section can define an LLM provider (`provider`, `base_url` for
  custom names, optional `model`, `api_key_env` naming the secret that holds
  the API key — store the key first with `save_secret`, never pass key
  material).
- `rename_agent {name}` — request a new agent name (separate action).

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
