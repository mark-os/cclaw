---
name: configuring-cclaw
description: How to read and change CClaw configuration — search_config, request_config actions (set_config, grants, rename), env-var precedence, and the config registry.
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
   `set_config`.
3. The registered default.

Keys flagged **secret** never resolve from the stored value: they resolve from
the environment or the encrypted secrets store only, and `set_config` refuses
them (ask the operator to use `save_secret` / the environment instead).

## Changing configuration

All writes go through `request_config`, which parks an approval for the
operator. Actions:

- `set_config {key, value, reason}` — change a registered config key. The key
  must exist in the registry; unknown keys fail immediately (no approval is
  parked), so check `search_config` first.
- `grant_tool {tool}` — request access to a tool you don't have.
- `grant_host {host}` — request network egress to a host (shell/web tools are
  default-deny).
- `grant_path {path, write}` — request filesystem access outside your
  workspace.
- `rename_agent {name}` — request a new agent name.

Give a concrete `reason` — the operator sees it in the approval prompt. If the
approval is denied, do not re-request the same thing; explain what you were
trying to do and let the operator decide.

## Channel configuration

Channels are configured through namespaced registry keys owned by their
extension: `<extension>.<key>` (e.g. `telegram.enabled`, `telegram.admin_ids`).
A channel launches only when its trust status is active, `<ext>.enabled` is
truthy, and every key flagged `required` resolves to a non-empty value.
Enabling a channel is therefore just `set_config` on `<ext>.enabled` (plus any
missing required keys).
