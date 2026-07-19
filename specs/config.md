# Config Resolution (design)

**Status: implemented (phase 4 of [self-configuration.md](self-configuration.md)).**

How operator-provided configuration gets into cclaw, expressed as one uniform
resolution rule instead of the current three ad-hoc mechanisms. Motivating
case: a wiped `cclaw.db` should come back fully configured from
`/etc/cclaw/env` alone — today that works for the provider API key and for
nothing else.

---

## The problem

`cclaw.db` mixes two kinds of state with opposite lifetimes:

- **Operator intent** — provider keys, channel credentials, admin ids, config
  overrides. Entered by a human once; painful to lose.
- **Runtime state** — sessions, entries, `tg_offset`, cron bookkeeping.
  Machine-generated; the whole point of "delete the DB" is to reset it.

"Delete the DB" as a reset is a policy about runtime state, but a wipe destroys
operator intent as collateral. Concretely, the Telegram channel needs three
operator inputs, and none has an external home:

| Input | Kind | Where it lives today |
|-------|------|----------------------|
| `bot_token` | secret | `channel_state` row, plaintext, hand-inserted |
| `admin_ids` | plain config | `channel_state` row, hand-inserted |
| "run this channel" | activation | `channels` row, hand-inserted (nothing recreates it) |

Meanwhile the provider key already has the right shape: the `providers` row
stores an env var *name* (`api_key_env`), and `config_load` resolves
`getenv(name)` first, encrypted kv second. The DB holds the declaration; the
env holds the value; a wipe loses nothing. This spec generalizes that pattern.

A second, related defect: `channel_state` is a parallel config store. Channel
config lives there instead of in the registry-backed `config` table, so it has
no defaults, no descriptions, no `search_config` discoverability, no
`config_set` validation, and no env story. "State has one home" — config's
home is the registry; `channel_state` should hold only runtime scratch.

## One resolution rule

Every registered config key `k` (core or `<ext>.<key>`) resolves as:

```
effective(k) = env(CCLAW_<K>)          -- live getenv, never copied to DB
             ?? config.value           -- operator/agent override (config_set)
             ?? config.default_value   -- code- or manifest-owned
```

`CCLAW_<K>` is mechanical: uppercase, `.` → `_`. So `web_port` →
`CCLAW_WEB_PORT`, `telegram.bot_token` → `CCLAW_TELEGRAM_BOT_TOKEN`. This
retroactively rationalizes the ad-hoc `CCLAW_*` overrides — they already
follow the mapping; the rule just makes it hold for every key including
extension keys, in one code path (`config_get`), instead of a hand-written
`getenv` per core knob.

The env layer is **read live, never written to the DB**. There is no seeding
step and therefore no staleness: edit `/etc/cclaw/env`, restart, done. A wiped
DB needs no reseeding because the env was never *in* the DB.

Precedence note: env outranks `config.value` because the env file is the
deployment's statement of intent (root-owned, survives wipes), while `value`
is runtime-mutable by agents. An agent cannot `config_set` its way past an
operator-pinned value.

## Secrets

Manifest `config[]` entries (and core registry entries) gain a `secret` flag:

```json
{ "key": "bot_token", "secret": true,
  "description": "Telegram bot API token" }
```

A secret key differs from a plain key in three ways:

1. **Storage** — never lands in `config.value`; `config_set` rejects it
   (the registry "holds no secrets" rule, now enforced per-key). The DB
   fallback is the encrypted `secrets` table under the key's canonical env
   name (`CCLAW_TELEGRAM_BOT_TOKEN`), exactly like `api_key_env` today:
   `getenv(name)` first, `db_secret_get_system(name)` second.
2. **Exposure** — `search_config`/`request_config` list the key but redact the
   value; the model refers to it as `{{SECRET:CCLAW_TELEGRAM_BOT_TOKEN}}` if
   it ever needs to pass it ([security.md](security.md)).
3. **DLP** — the resolved value registers with the secret scanner so it can't
   enter the context window through a tool result.

`providers.api_key_env` is the existing instance of this pattern and stays
as-is; re-expressing providers as registry keys is out of scope.

## Channels read config through the registry

The channel JS API splits into config (registry, read-only) and state
(`channel_state`, read-write):

| API | Backing | Notes |
|-----|---------|-------|
| `cclaw.getConfig(key)` | registry key `<channel-ext>.<key>` via the resolution rule | read-only from JS; secrets resolve (the channel process needs the token) but are scanner-registered |
| `cclaw.getState(key)` / `setState(key, value)` | `channel_state` kv | runtime scratch only: `tg_offset`, webhook registration state |

The Telegram manifest declares its surface:

```json
"config": [
  { "key": "enabled",        "default": "0",
    "description": "Run the telegram channel (1 = on)" },
  { "key": "bot_token",      "secret": true, "required": true,
    "description": "Telegram bot API token" },
  { "key": "admin_ids",      "default": "",
    "description": "Comma-separated Telegram user ids with admin commands" },
  { "key": "base_url",       "default": "https://api.telegram.org",
    "description": "Bot API base URL (egress pin source of truth)" },
  { "key": "webhook_secret", "secret": true, "default": "",
    "description": "X-Telegram-Bot-Api-Secret-Token for webhook mode" }
]
```

`channel_state` keeps only `tg_offset`. The hand-seeded `base_url` insert in
`extract_builtin_extensions` disappears — it's a manifest default now.

## Activation is a config key

"Deciding to set up Telegram" stops being a hand-inserted `channels` row and
becomes a registry key like any other:

- **Row creation**: the builtin telegram bundle gets a real `extension.json`
  and goes through the same manifest ingest as any promoted extension
  ([extensions.md](extensions.md)), which creates the `channels` row. Builtin
  bundles are shipped code, already past the trust gate, so their row is born
  trust-`active`; agent-authored channels keep the draft → validated → active
  lifecycle unchanged. Trust state says *may this code run* — never *should
  it*.
- **Enablement**: every channel extension declares (or is auto-registered
  with) an `enabled` config key, **default `0`** — a builtin present in every
  install must not run unless asked; not everyone uses Telegram. It resolves
  through the standard rule, so turning the channel on is
  `CCLAW_TELEGRAM_ENABLED=1` in `/etc/cclaw/env` (or
  `config_set telegram.enabled 1` at runtime).
- **Launch gate**: `channel_launch_all` launches a channel iff its trust
  status is `active` **and** `<ext>.enabled` resolves true **and** all
  `required` config keys resolve; otherwise it skips with a log line naming
  what's missing.

This keeps the two axes separate: `channels.status` is trust lifecycle only
(no more hand-set `disabled` overloading it), and operator intent — on/off
plus credentials — lives entirely in config, all of it expressible in the env
file.

## What a wipe looks like after this

`/etc/cclaw/env`:

```sh
OPENROUTER_API_KEY=sk-or-v1-...
CCLAW_TELEGRAM_ENABLED=1
CCLAW_TELEGRAM_BOT_TOKEN=123456:ABC-...
CCLAW_TELEGRAM_ADMIN_IDS=1200505293
```

Fresh DB → builtin extract writes the bundle → manifest ingest registers
extension + config keys + `channels` row → launch gate sees `enabled` true
and the required key resolved from env → channel up. Zero manual steps. Non-env `config.value`
overrides still die with the DB — acceptable during the wipe era, and moot
once the DB becomes durable. Post-durability, the env layer's remaining jobs
are secrets (credentials shouldn't have the DB as their only home anyway) and
first-boot seeding.

## Touch points when implemented

- `config_get`: add the env layer (one mechanical name mapping).
- Manifest ingest: `secret` / `required` flags on `config[]`; `config` table
  grows matching columns.
- `config_set`: reject `secret` keys.
- QJS channel host: `getConfig` reads the registry; add `getState`/`setState`.
- Builtin telegram: ship `extension.json`; delete the hand-seeded `base_url`
  insert; `tg_offset` moves to `getState`.
- `channel_launch_all`: `enabled` + required-config gate.
- `templates/cclaw.init`: replace the dangling `CCLAW_TELEGRAM_TOKEN` export
  with the mechanical names (or source-and-export-all).
