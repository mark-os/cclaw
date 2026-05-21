# FUTURE — Deferred Ideas

Parking lot for features explicitly out-of-scope in current SPEC.md.
Move to §T when ready to implement.

## WhatsApp Business API Channel
- Webhook endpoint via civetweb: `POST /webhook/whatsapp`
- Verify endpoint: `GET /webhook/whatsapp?hub.verify_token=...`
- Send via Graph API (`POST https://graph.facebook.com/v21.0/{phone_id}/messages`)
- phone_number → session_id mapping

## Web Dashboard (Full)
- Session list, message viewer, sub-agent status, cron jobs
- Web-based chat interface
- Simple HTML — no JS framework, server-rendered or minimal vanilla JS

## Branching UI
- Visual tree in web interface w/ user-selectable branching
- Optional summarization on branch navigate (Pi model: summarize abandoned branch)
- Pi pattern: `collectEntriesForBranchSummary` → find common ancestor → summarize divergent path
- Branch summary stored as entry type in session tree

## Session Curation Agent
- Autonomous agent that periodically performs branching/compaction for other agents
- Summarizes old branches, prunes dead ends
- Runs as sub-agent on schedule (cron)

## LLM-based Compaction
- When context window fills, summarize old messages via LLM call
- Pi pattern: `shouldCompact()` when `contextTokens > contextWindow - reserveTokens`
- Keep ~20k recent tokens, summarize the rest
- Iterative: update previous summary rather than re-summarize everything
- Cut at valid turn boundaries (never mid-tool-call)
- Store as `compaction` entry in session tree

## Cross-compile for Pogoplug
- Target: Pogoplug V4 (ARMv5TE, 128MB RAM, Debian Bookworm armel)
- musl static link or armel dynamic
- Reduced feature set (no mquickjs? smaller arena?)

## Multi-model Routing
- Different models for different tasks (cheap for compaction, expensive for reasoning)
- Per-session model override stored in DB

## FTS5 Search Tool
- Expose message search as agent tool (`search_history`)
- Agent can search its own past conversations

## Landlock Shell Restriction
- Use Linux Landlock LSM to restrict `shell_exec` filesystem access
- Limit to workspace dir + explicit allowlist (e.g., `/usr/bin`, `/tmp`)
- Prevents agent from escaping workspace via shell even without path checks
- Reference: OpenClaw's crabbox sandbox model
