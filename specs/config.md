# Config Resolution

How operator-provided configuration gets into cclaw: one resolution rule for
every registered key. Design goal: a wiped `cclaw.db` comes back fully
configured from `/etc/cclaw/env` alone.

---

## Rationale

`cclaw.db` holds two kinds of state with opposite lifetimes:

- **Operator intent** — provider keys, channel credentials, admin ids, config
  overrides. A human enters them once; losing them is expensive.
- **Runtime state** — sessions, entries, `tg_offset`, cron bookkeeping.
  Machine-generated; deleting the DB resets it on purpose.

The DB holds the *declaration* of a key (name, default, description, flags).
The env file can hold the *value*. A wipe then loses no operator intent that
was put in the env file. The provider key was the first instance of this
pattern: the `providers` row stores an env var *name* (`api_key_env`), and
`config_load` resolves `getenv(name)` first and the encrypted `secrets` row
second.

Config has one home: the registry-backed `config` table. `channel_state`
holds only runtime scratch, never configuration.

## One resolution rule

Every registered config key `k` (core or `<ext>.<key>`) resolves as:

```
effective(k) = env(CCLAW_<K>)          -- live getenv, never copied to DB
             ?? config.value           -- operator/agent override (config_set)
             ?? config.default_value   -- code- or manifest-owned
```

`CCLAW_<K>` is mechanical: uppercase, `.` → `_`. So `web_port` →
`CCLAW_WEB_PORT`, `telegram.bot_token` → `CCLAW_TELEGRAM_BOT_TOKEN`. The rule
holds for every key, including extension keys, in one code path
(`src/config_registry.c`).

The env layer is **read live, never written to the DB**. There is no seeding
step and therefore no staleness: edit `/etc/cclaw/env`, restart, done. A wiped
DB needs no reseeding because the env was never *in* the DB.

Precedence note: env outranks `config.value` because the env file is the
deployment's statement of intent (root-owned, survives wipes), while `value`
is runtime-mutable by agents. An agent cannot `config_set` its way past an
operator-pinned value.

## Secrets

Manifest `config[]` entries (and core registry entries) can carry a `secret` flag:

```json
{ "key": "bot_token", "secret": true,
  "description": "Telegram bot API token" }
```

A secret key differs from a plain key in three ways:

1. **Storage** — never lands in `config.value`; `config_set` rejects it
   (the registry holds no secrets). The DB fallback is the encrypted
   `secrets` table (scope `system`) under the key's canonical env name
   (`CCLAW_TELEGRAM_BOT_TOKEN`), the same as `api_key_env`: `getenv(name)`
   first, `db_secret_get_system(name)` second.
2. **Exposure** — `search_config`/`request_config` list the key but redact the
   value; the model refers to it as `{{SECRET:CCLAW_TELEGRAM_BOT_TOKEN}}` if
   it ever needs to pass it ([security.md](security.md)).
3. **DLP** — the resolved value registers with the secret scanner so it can't
   enter the context window through a tool result.

`providers.api_key_env` is the other instance of this pattern. Providers are
not registry keys.

## Channels read config through the registry

The channel JS API splits into config (registry, read-only) and state
(`channel_state`, read-write). Both hang off the `channel` global:

| API | Backing | Notes |
|-----|---------|-------|
| `channel.getConfig(key)` | registry key `<channel-ext>.<key>` via the resolution rule | read-only from JS; secrets resolve (the channel process needs the token) but are scanner-registered |
| `channel.getState(key)` / `channel.setState(key, value)` | `channel_state` kv | runtime scratch only: `tg_offset`, webhook registration state |

The Telegram manifest (`templates/channel_telegram_manifest.json`) declares its surface (abridged):

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

For Telegram, `channel_state` holds only `tg_offset`. `base_url` is a manifest
default.

## Activation is a config key

Turning a channel on is a registry key like any other:

- **Row creation**: a builtin channel bundle has a real `extension.json` and
  goes through the same manifest ingest as any promoted extension
  ([extensions.md](extensions.md)), which creates the `channels` row. Builtin
  bundles are shipped code, already past the trust gate, so a fresh install
  creates their row as `active`; agent-authored channels follow the
  draft → validated → active lifecycle. Trust state says *may this code run* — never *should
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

This keeps the two axes separate: `channels.status` is trust lifecycle only,
and operator intent — on/off plus credentials — lives entirely in config, all
of it expressible in the env file.

## What a wipe looks like

`/etc/cclaw/env`:

```sh
OPENROUTER_API_KEY=sk-or-v1-...
CCLAW_TELEGRAM_ENABLED=1
CCLAW_TELEGRAM_BOT_TOKEN=123456:ABC-...
CCLAW_TELEGRAM_ADMIN_IDS=123456789
```

Fresh DB → builtin extract writes the bundle → manifest ingest registers
extension + config keys + `channels` row → launch gate sees `enabled` true
and the required key resolved from env → channel up. No manual steps.
`config.value` overrides that are not in the env file are lost with the DB.
