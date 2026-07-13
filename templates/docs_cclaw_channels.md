---
name: cclaw-channels
description: How CClaw channels work — the channel extension component, activation via config keys, the inbox/outbox flow, and the QuickJS channel API.
---

# CClaw Channels

A channel connects CClaw to an outside surface (Telegram, a webhook, a custom
feed). Channels are shipped as the `channel` component of an extension: a
QuickJS handler file plus config keys, run by the daemon in a dedicated
runner.

## Anatomy

- `channels` table row: channel name → `extension_name`; `status` is its trust
  state (draft → active).
- Config: namespaced registry keys `<extension>.<key>`. Convention:
  `enabled` (launch switch), `base_url` (egress pin — the runner refuses sends
  to any other host), plus whatever the handler needs. Secret keys (tokens)
  resolve from env / the encrypted secret store only.
- State: `channel_state` rows are the channel's private runtime scratch
  (poll offsets, feature latches) via `getState`/`setState`.

## Launch gate

The daemon launches a channel only when **all** hold:

1. trust status is `active` (promotion / operator activation),
2. `<ext>.enabled` resolves truthy,
3. every config key flagged `required` resolves non-empty.

So "turn on telegram" = ensure `telegram.bot_token` is provided (env
`CCLAW_TELEGRAM_BOT_TOKEN` or secret store) and set `telegram.enabled=1`.

## Handler contract (channel.qjs)

The handler exports lifecycle functions; the C runner owns the loop, HTTP, and
retries:

- `onInit()` — read config, return `{poll: {method,url,…}}` to start polling.
- `onPoll(result)` — parse the poll response, `channel.emit('message', json)`
  for each inbound item, return the next poll shape.
- `onOutbox(item)` — deliver one outbound item: build the send with
  `channel.send({method,url,body, outbox_id: item.id, final: 1})`.
- `onRequest(req)` — handle webhook/UDS HTTP requests; return
  `{status, body}`.

Host API: `channel.getConfig(key)` (registry, read-only),
`channel.getState/setState(key, value)` (runtime scratch), `channel.emit`,
`channel.send`, `channel.http`, `channel.log`, `channel.ackOutbox/failOutbox`,
and `channel.admin.isAdmin(id)` / `channel.admin.listPendingApprovals()` /
`channel.admin.dashboardUrl()`.

## Message flow

Inbound: handler `emit` → `channel_events` → the daemon routes to an agent
session (a message from a channel arrives to you as a normal user turn).
Outbound: your reply is written to `channel_outbox` → the runner wakes,
calls `onOutbox`, delivers, and acks. You never call the network yourself for
channel traffic — reply normally and the channel delivers it.
