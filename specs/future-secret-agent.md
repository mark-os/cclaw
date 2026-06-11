# Future: Secret Agent + Trusted Execution Path

Design proposal for automated secret management — a dedicated agent that can
receive, store, and provision secrets without exposing them to the main agent
context window.

## Problem

Users need to add secrets (API keys, bot tokens, service credentials) to cclaw.
Current flow requires manual `CCLAW_SECRET_*` env vars. Ideally:

1. User says "sign me up for service X" or "here's my GitHub token"
2. The secret never appears in the main agent's context
3. It's stored encrypted and available via `{{SECRET:name}}` immediately

## Proposed Architecture

```
User: "Here's my token: ghp_abc123..."
          │
          ▼ AC scanner detects secret in user message
          │
          ▼ Route to Secret Agent (ZDR model)
          │
┌─────────────────────────────────────────┐
│ Secret Agent                            │
│ - trust_level = "secret_agent" (new)    │
│ - Model: ZDR endpoint (zero data ret.)  │
│ - Tools: secret_store_write, confirm    │
│ - Context: NEVER persisted to entries   │
│                                         │
│ 1. Receives the raw message             │
│ 2. Extracts secret name + value         │
│ 3. Writes to encrypted secret store     │
│ 4. Returns confirmation to main session │
└─────────────────────────────────────────┘
          │
          ▼ Main agent sees: "Secret 'GITHUB_TOKEN' stored successfully.
                              Use {{SECRET:GITHUB_TOKEN}} to reference it."
```

## Key Design Decisions

### ZDR / TEE Execution

The secret agent routes to a model endpoint that guarantees zero data retention:

- **ZDR providers**: Anthropic (with ZDR agreement), Azure OpenAI (data at rest off), self-hosted models
- **TEE**: Future option — run the model in an SGX/TDX enclave, attestation proves no logging
- **Fallback**: Local model (llama.cpp) on the same machine — never leaves the box

### Trigger: When to Route to Secret Agent

1. **AC scanner fires on user input** — if a user message contains a detected secret pattern, offer to route it to the secret agent instead of storing it in the session
2. **Explicit command** — user says "store secret" or similar intent
3. **Bootstrap flow** — during service setup, the bootstrap agent explicitly hands off to the secret agent for credential collection

### Secret Store Write API

```c
/* Only callable by agents with trust_level = "secret_agent" */
int secret_store_write(sqlite3 *db, const char *name, const char *plaintext,
                       const char *scope);  /* scope: agent name or "*" */
```

- Encrypts with the existing `.cclaw_key` (ChaCha20-Poly1305)
- Stored in cclaw.db `secrets` table
- Available via `CCLAW_SECRET_<name>` env injection and `{{SECRET:name}}`

### Per-Agent Secret Scoping

Secrets can be scoped:
- `"*"` — available to all agents
- `"agent_name"` — only that agent gets it injected
- `"agent1,agent2"` — comma-separated list

### Interaction with Existing System

- `{{SECRET:name}}` interpolation works the same — the secret agent just provides another way to *add* secrets
- AC scanner still catches leaks — belt and suspenders
- Existing `CCLAW_SECRET_*` env var injection is the delivery mechanism

## Service Signup Flow (OpenClaw-style)

```
User: "Set up a Telegram bot for me"
  │
  ▼ Bootstrap agent guides user through BotFather
  │
  ▼ User pastes bot token: "7891234567:AAF..."
  │
  ▼ AC scanner detects telegram-bot-api-token pattern
  │
  ▼ Instead of storing in context, route to secret agent
  │
  ▼ Secret agent:
      - Stores as {{SECRET:TELEGRAM_BOT_TOKEN}}
      - Calls configure_channel with the reference
  │
  ▼ Main session sees: "Telegram bot configured ✓"
```

## Open Questions

1. **Rotation/expiry** — should secrets have TTL? Notification when expired?
2. **Audit trail** — log secret access (which agent used which secret, when)?
3. **Multi-secret transactions** — OAuth flows need client_id + client_secret + redirect together
4. **User confirmation** — always confirm before storing? Or auto-store on detection?
5. **Secret deletion** — who can delete? Any agent, or only secret_agent?

## Non-Goals (for now)

- HSM integration (key stays in `.cclaw_key` file)
- Secret sharing across multiple cclaw instances
- Automatic secret rotation with providers
- RBAC on secret access (simple scope string for now)

## Implementation Hooks (already in place)

- AC scanner detects secrets in user input → could trigger routing
- `{{SECRET:name}}` interpolation → delivery mechanism works
- Encrypted secret store in cclaw.db → storage ready
- `shell_secrets_collect()` → injection mechanism works
- Trust-level system → can gate secret_store_write to specific agents
