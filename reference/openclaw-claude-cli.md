# OpenClaw Claude CLI Backend — Reference

How OpenClaw uses a Claude subscription: it shells out to `claude -p` and lets
Claude Code run its own tools. Read from the local checkout
(`reference/openclaw`, commit `f1076c23`, 2026-07-13); it may have changed
since. The other design, where the host keeps the tools, is OpenCode's and is
in [specs/subscription-providers.md](../specs/subscription-providers.md).

Source: `extensions/anthropic/cli-backend.ts`,
`extensions/anthropic/cli-shared.ts`, `src/agents/cli-runner/`.

## The command

`-p` is `--print`: Claude Code's non-interactive mode. One prompt in on stdin,
the run goes to completion, the process exits.

```
claude -p --output-format stream-json --include-partial-messages --verbose
       --setting-sources user
       --allowedTools "mcp__openclaw__*"
       --disallowedTools "ScheduleWakeup,CronCreate,Bash(run_in_background:true),Monitor"
       [--permission-mode bypassPermissions]
       [--session-id <id> | --resume <id>] [--fork-session]
       [--model <alias>] [--append-system-prompt-file <path>]
       [--mcp-config <file>]
```

- Output is JSON lines (`stream-json`); OpenClaw reads events as they happen.
- The session id is always passed (`sessionMode: "always"`), so a later turn
  resumes the same Claude session with `--resume`.
- OpenClaw's system prompt is appended (`--append-system-prompt-file`). Claude
  Code's own prompt stays.
- Only the user-level Claude settings load (`--setting-sources user`).
- Runs are serialized (`serialize: true`).

## Who runs the tools

Claude Code does. Its built-in tools stay on (`nativeToolMode: "selectable"`).
A shell command is executed by the `claude` process, on the host, with the
daemon user's rights. A pipeline is not special: `cat x | grep y` is one Bash
command string handed to a shell. OpenClaw does not parse it, approve it or
sandbox it. It sees the command only in the event stream, after it has run.

OpenClaw's own tools are added beside the built-in ones as an MCP server
(`bundleMcp: true`, written to a config file and passed with `--mcp-config`),
and are pre-approved by `--allowedTools "mcp__openclaw__*"`.

## Permissions

The permission mode follows OpenClaw's exec setting
(`resolveClaudePermissionMode`, `cli-shared.ts`):

```ts
const security = exec?.security ?? "full";
const ask = exec?.ask ?? "off";
return security === "full" && ask === "off";   // → bypassPermissions
```

Both defaults are the permissive ones, so by default the flag is
`--permission-mode bypassPermissions` and Claude Code asks nobody. When the
operator tightens `tools.exec` (globally or for one agent), OpenClaw passes no
permission mode and Claude Code's normal rules apply. The old
`--dangerously-skip-permissions` flag is stripped from configured arguments.

## What is blocked

Only the tools that would outlive a one-shot run: `ScheduleWakeup`,
`CronCreate`, `Monitor`, and Bash with `run_in_background`.

## Other details

- Claude Code owns compaction (`ownsNativeCompaction: true`).
- A list of environment variables is cleared before the spawn
  (`CLAUDE_CLI_CLEAR_ENV`), telemetry exporters among them.
- A watchdog kills a run that stops producing output; fresh and resumed runs
  have separate limits.
- Images are passed as `@path` references, limited to the workspace.

## Compared with OpenCode

| | OpenClaw | OpenCode |
|---|---|---|
| How `claude` is started | `claude -p`, one process per turn | `--input-format stream-json` through the Agent SDK, one process per session |
| Claude Code's built-in tools | on | off (`--tools ""`) |
| Who runs a shell command | the `claude` process | the host |
| Who approves it | nobody by default (bypass mode) | the host's permission system |
| The host's own tools | added over MCP | the only tools, over MCP |
| History | Claude Code's session; the host reads the stream | the host's database; Claude gets a checkpoint of what it missed |
| Compaction | Claude Code's | the host's |

## Why cclaw would not copy this

The model gets an unsupervised shell outside the namespace sandbox, the egress
proxy, grants, approvals and the secret scanner. Every boundary in
`specs/security.md` is on the far side of the `claude` process. It is also the
cheap design: no pipe protocol to keep open, no MCP round trip per call, and
no history sync.
