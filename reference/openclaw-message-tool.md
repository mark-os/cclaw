# OpenClaw Message Tool — Reference

## Two Reply Paths

1. **Implicit reply** — model returns final assistant text, OpenClaw auto-delivers to the session's originating channel. No tool call needed.
2. **Explicit `message` tool** — agent calls `message(action=send, ...)` to post deliberately.

Controlled by `visibleReplies`:
- `"automatic"` — final text auto-delivered (default for DMs)
- `"message_tool"` — final text stays private, agent must call tool to be visible (default for groups)

## The `message` Tool

Single polymorphic tool whose schema is dynamically assembled from active channel plugins. Each channel plugin contributes via `describeMessageTool()`:
- Available **actions** (send, react, pin, kick, etc.)
- **Schema fragments** (channel-specific parameters)
- **Capabilities** declared in config

From the model's perspective: one tool, union of all channel capabilities.

## Actions (channel-dependent)

- `send` — post a message
- `react` — add a reaction
- `pin` / `unpin`
- `kick`
- Channel-specific actions (iMessage tapbacks, Discord threads, etc.)

## Routing

Sessions track their origin channel (`lastChannel` / `lastTo`). The message tool can:
- Reply on the current session's channel (implicit target)
- Send cross-channel (explicit `channel`, `target` params)

## Per-Channel Gating

Config-driven capability control, not runtime approval:

```
channels.<provider>.actions.messages: true/false
channels.<provider>.actions.reactions: true/false
channels.<provider>.actions.pins: true/false
channels.matrix.actions.verification: true/false
```

Per-group overrides:
```
channels.signal.groups["<group-id>"].tools
channels.discord.tools
channels.msteams.teams.<teamId>.channels.<id>.tools
```

Per-sender restrictions:
```
channels.<provider>.toolsBySender.<key>  (channel:, id:, e164:, username:, name:, "*")
```

## Visibility Modes

| Context | Default `visibleReplies` | Behavior |
|---------|--------------------------|----------|
| Direct messages | `"automatic"` | Final text auto-posted |
| Group chats | `"message_tool"` | Agent lurks; must call tool to speak |

In `message_tool` mode, the agent still processes turns (updates memory, calls other tools) but suppresses final text from appearing in the room.

## Schema Assembly

Channel plugins implement `ChannelMessageToolDiscoveryAdapter`:
```typescript
describeMessageTool(ctx: ChannelMessageActionDiscoveryContext): {
  actions: string[];
  schema?: ChannelMessageToolSchemaContribution;
  capabilities?: ChannelMessageCapability[];
}
```

Core merges contributions from all configured channels into the single `message` tool definition sent to the model.

## Key Differences from CClaw

- OpenClaw: one `message` tool, polymorphic across channels, schema assembled at runtime
- CClaw: channels are separate JS extension programs that poll/send independently; the agent's reply is delivered by the channel runner, not a tool call

## No Per-Message Approval

There is no interactive "approve this send?" flow. Outbound capability is controlled by:
1. Channel config (actions enabled/disabled)
2. Tool policy (allow/deny lists)
3. Delegate tier (read-only / send-on-behalf / proactive)
4. Standing orders in AGENTS.md (advisory guardrails)
