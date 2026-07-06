# Telegram Bot API — Research Notes

Research from OpenClaw (TS/grammY, 331-file plugin) and Hermes (Python/PTB, ~6500-line adapter).

## Mini Apps vs Bots

**Bots** — server-controlled Telegram accounts. Users interact via chat: text, commands, inline keyboards, callback queries. Speak the Bot API (`sendMessage`, `editMessageText`, `answerCallbackQuery`). This is what cclaw uses.

**Mini Apps** (formerly "Web Apps") — full HTML/CSS/JS apps that open inside Telegram's built-in WebView. Launched via a `web_app` button in an inline keyboard. Get Telegram user identity, can use TON payments, provide arbitrary custom UI. Think iframe inside Telegram — the bot is still the host.

OpenClaw supports Mini App buttons (`web_app: { url }`) as one button type in inline keyboards but doesn't use them for core agent interaction. For cclaw's use case (approvals, config, admin), mini apps are massive overkill. Inline keyboards + `editMessageText` + `answerCallbackQuery` is the right layer.

## Bot API UI Layers

| Layer | What it does | Used by cclaw? |
|-------|-------------|----------------|
| Inline keyboards | Buttons attached under a message (`reply_markup: {inline_keyboard}`) | Yes |
| Force-reply | Auto-focuses input with reply hint | Yes |
| `setMyCommands` | Registers persistent `/` command menu | **No** (stale NullClaw menu) |
| Bot command scopes | Per-chat/per-user command menus | No |
| Reply keyboards | Replaces client keyboard with custom buttons (persistent, clunky) | No |
| `editMessageText` | Update message in place | No |
| `editMessageReplyMarkup` | Update only the buttons on a message | No |
| `answerCallbackQuery` with text | Show toast popup without new message | Partially (dismisses spinner only) |
| `sendMessageDraft` (Bot API 9.5) | Native streaming preview in DMs | No |
| `sendRichMessage` (Bot API 10.1) | Send raw markdown with native table/task-list rendering | No |
| DM Topics (Bot API 9.4) | Forum topics inside private DM chats | No |
| Reactions | Bot sends emoji reaction on message | No |

## Approval Button Pattern (proven by both projects)

Both OpenClaw and Hermes converged on the same design:

### Send the prompt

```
⚠️ Command Approval Required

<pre>rm -rf /important</pre>

Reason: dangerous deletion

[✅ Allow Once] [✅ Session]
[✅ Always]     [❌ Deny]
```

- HTML parse mode (`parse_mode: "HTML"`)
- `reply_markup: { inline_keyboard: [[...], [...]] }` — 2×2 button grid
- Callback data format: `ea:<choice>:<approval_id>` (kept under 64 bytes — Telegram hard limit)

### Handle the click

1. **Auth check** — verify the clicker is authorized (fail-closed). Don't trust visibility = permission.
2. **`answerCallbackQuery`** with `text: "✅ Approved once"` — shows toast, dismisses loading spinner.
3. **`editMessageText`** — replace the original prompt with outcome: `"✅ Approved once by Alice"`, set `reply_markup: null` to remove stale buttons.
4. **Resolve the approval** — unblock the waiting agent thread.
5. **Resume typing** — Hermes discovered that typing indicator gets paused while waiting for approval and must be explicitly resumed after resolution.

### Key constraints

- `callback_data` max: **64 bytes** (UTF-8). Use short prefixes: `ea:once:1`, `ea:deny:2`.
- Buttons are only clickable while the message exists and hasn't been edited to remove them.
- Multiple users can see and click the same buttons — auth check on every callback is mandatory.
- Approval state is keyed by a monotonic counter ID, mapped to session_key in adapter memory.
- Hermes expires approvals after 30 minutes by default.

## `setMyCommands` — Fixing the Stale Menu

Both projects register commands at startup:

```
setMyCommands(commands, scope)
```

Scopes (narrowest match wins):
- `BotCommandScopeDefault` — fallback for all chats
- `BotCommandScopeAllPrivateChats` — all DMs
- `BotCommandScopeAllGroupChats` — all groups
- `BotCommandScopeChat(chat_id)` — specific chat (needed for forum supergroups)

Telegram allows up to 100 commands but has an undocumented ~4KB payload limit. OpenClaw caps at 30 commands per scope. Forum topics don't inherit `AllGroupChats` — need per-chat registration on first message (lazy).

Errors:
- `BOT_COMMANDS_TOO_MUCH` — too many entries, reduce count
- `404: Not Found` — check `apiRoot` isn't set to the full `/bot<TOKEN>` endpoint
- `getMe returned 401` — bad token

## `editMessageText` — Live Streaming

Both projects use the same pattern for streaming LLM responses:

1. Send a placeholder message (`sendMessage` with partial text)
2. Edit it repeatedly as tokens arrive (`editMessageText`)
3. Final edit with formatted text (MarkdownV2 or HTML)

**Hermes additionally supports:**
- `sendMessageDraft` (Bot API 9.5, DMs only) — native animated preview, no message_id needed, caller passes a `draft_id` that Telegram animates between frames
- `sendRichMessage` (Bot API 10.1) — sends raw markdown that Telegram renders natively (tables, task lists, collapsible sections)
- Overflow split: when content exceeds 4096 chars, edit first chunk in place + send continuations as replies

**OpenClaw streaming modes:**
- `partial` — edit preview message, finalize in place (one message visible to user)
- `progress` — status draft for tool progress, clear it, send final as fresh message
- `block` — chunk-based streaming
- `off` — final-only delivery

Key lesson: "Message is not modified" is a no-op, not an error. Handle it gracefully.

## Notification Control

Hermes defaults to "important" mode: all sends use `disable_notification: true` except final responses, approvals, and slash confirmations. This prevents per-tool-call push notification spam. Configurable per-adapter.

## Status Message Deduplication

Hermes's `send_or_update_status()`: first call sends a message and caches `(chat_id, status_key) → message_id`. Subsequent calls with the same key edit that same message. Prevents progress/status spam from appending new bubbles on every update.

## Ack Reactions

Hermes sends an emoji reaction (default 👀) on message receipt while processing. Resolution order: per-account config → channel config → global messages config → agent identity emoji → "👀". Use `""` to disable.

## Text Batching (Hermes)

Telegram clients split messages over 4096 chars into multiple updates. Hermes buffers rapid successive texts from the same user/chat with adaptive delay (180ms for short messages, up to 1s for split-threshold messages) and aggregates into a single event. Prevents self-interrupting multiple agent turns from one logical user message.

## DM Topics (Bot API 9.4)

Telegram now supports forum topics inside private DM chats. Hermes uses this to organize conversation lanes within a single DM (like having named sub-threads). Created via `createForumTopic(chat_id, name)`. User must enable "Topics" in their chat settings with the bot first.

## Forum Topics

Both projects support forum supergroups with per-topic session isolation:
- Session keys append `:topic:<threadId>`
- General topic (`threadId=1`) is special: `sendMessage` must omit `message_thread_id` (Telegram rejects it), but `sendChatAction` (typing) needs it
- Per-topic config: agent routing, allowlists, mention requirements

## Group Mention Gating

Both projects implement require-mention for groups:
- Bot must be @mentioned, replied-to, or match a regex wake-word pattern
- `exclusive_bot_mentions` — when multiple bots are in a group, explicit @bot mentions exclusively route (prevents cross-talk)
- Unmentioned messages can be "observed" (stored in session transcript as context) without triggering a response

## Access Control

- `allowFrom` uses **numeric Telegram user IDs** (not usernames — those are mutable)
- DM pairing is separate from group authorization
- `callback_data` clicks need independent auth checks
- Telegram's privacy mode limits what group messages bots receive — must be disabled or bot made admin for full visibility

## Connection Resilience (Hermes)

- Fallback IP transport — auto-discovers Telegram API IPs, uses them when primary DNS fails
- Exponential backoff on network errors (5s → 60s cap, 10 retries)
- Polling conflict handling (409) — backs off 15-55s for server-side session expiry
- Pool timeout detection — httpx pool exhausted is safe to retry (request never left the process)
- Post-reconnect heartbeat probe — detects wedged polling after ostensibly-successful reconnect
- `send_path_degraded` flag — marks adapter unhealthy after sustained reconnect storms

## Telegram Hard Limits

| Limit | Value |
|-------|-------|
| Message text | 4096 UTF-16 code units |
| `callback_data` | 64 bytes UTF-8 |
| Caption | 1024 chars |
| Bot commands per scope | 100 (practical: ~30 due to payload size) |
| Native quote text | 1024 UTF-16 code units |
| Media group | 10 items |
| `getFile` download | 20 MB (public API), 2 GB (local bot API server) |
| Rich message text (Bot API 10.1) | 32,768 UTF-8 bytes |
| Poll options | 12 |

## What cclaw Should Steal

For the approval-button redesign:
1. `editMessageText` + `reply_markup: null` to resolve prompts cleanly
2. `answerCallbackQuery` with toast text for immediate feedback
3. Short `prefix:choice:id` callback_data format (< 64 bytes)
4. Auth-check every callback click (fail-closed)
5. Resume typing after approval resolution

For general Telegram quality:
1. `setMyCommands` at startup — fix the stale menu
2. `disable_notification: true` for non-final messages (reduce noise)
3. Status message deduplication (edit in place, don't spam bubbles)
4. Ack reaction on receipt (👀) while processing
5. Handle "Message is not modified" as success, not error
