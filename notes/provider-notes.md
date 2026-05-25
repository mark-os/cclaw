# Provider Auth & Access Notes

## OpenAI / ChatGPT (via OpenClaw)

OpenClaw supports three auth methods for OpenAI:

### 1. API Key (traditional)
- Set `OPENAI_API_KEY` env var
- Direct billing to your OpenAI account
- No OAuth, no expiry

### 2. ChatGPT Browser OAuth (PKCE + localhost callback)
- Full OAuth 2.0 Authorization Code flow with PKCE
- Opens browser → `auth.openai.com/oauth/authorize`
- Local callback server on `localhost:1455`
- Returns access + refresh tokens
- Uses hardcoded client ID: `app_EMoamEEZ73f0CkXaXp7hrann`
- Originator: `"openclaw"`
- Fallback: if callback doesn't fire, user pastes redirect URL manually
- Remote/VPS mode: shows URL for user to open on local machine
- TLS preflight check before starting (catches cert issues early)
- Region-restricted: `unsupported_country_region_territory` error if blocked

### 3. ChatGPT Device Code Pairing
- OAuth 2.0 Device Authorization Grant (RFC 8628)
- No browser redirect needed — works on headless/remote machines
- Flow:
  1. POST `auth.openai.com/api/accounts/deviceauth/usercode` → get device_auth_id + user_code
  2. Show user: "Go to auth.openai.com/codex/device and enter code: XXXX-XXXX"
  3. Poll `auth.openai.com/api/accounts/deviceauth/token` every 5s (403/404 = pending)
  4. On success: get authorization_code + code_verifier
  5. Exchange at `auth.openai.com/oauth/token` (grant_type=authorization_code) → access + refresh tokens
- Timeout: 15 minutes
- Same client ID as browser OAuth

### Difference between the two ChatGPT flows:
- **Browser OAuth**: Requires a browser that can reach localhost:1455. Faster (one click). Best for local dev machines.
- **Device Code**: No browser callback needed. User goes to a URL and types a code. Best for headless servers, SSH sessions, containers, embedded devices.

Both give you the same access/refresh token pair. Both let ChatGPT Plus/Pro subscribers use OpenAI models without a separate API key.

**OpenAI has NOT banned accounts for using these flows.** They provide the auth endpoints and client IDs explicitly for this purpose (Codex product).

---

## Anthropic

OpenClaw supports two auth methods:

### 1. API Key
- Set `ANTHROPIC_API_KEY` env var
- Direct billing

### 2. OAuth / Setup Token
- `ANTHROPIC_OAUTH_TOKEN` env var
- "Setup token" flow: user pastes a token from Anthropic console
- Token has expiry (configurable)
- Used for Claude Pro/Team subscription access (similar to ChatGPT OAuth — use your subscription instead of API billing)

---

## Google Gemini

### 1. API Key
- Set `GEMINI_API_KEY` env var
- Sent as `x-goog-api-key` header
- Free tier generous, paid tier available

### 2. Gemini CLI OAuth (PKCE + localhost)
- **⚠️ CAUTION: Google has been banning/suspending accounts**
- OpenClaw shows explicit warning: "Some users have reported account restrictions or suspensions after using third-party Gemini CLI and Antigravity OAuth clients"
- Requires user confirmation before proceeding
- Uses `OPENCLAW_GEMINI_OAUTH_CLIENT_ID` / `OPENCLAW_GEMINI_OAUTH_CLIENT_SECRET` env vars (or `GEMINI_CLI_OAUTH_CLIENT_ID`)
- Returns OAuth token stored as JSON: `{"token": "...", "projectId": "..."}`
- Sent as `Authorization: Bearer <token>` header

### Why Google bans but OpenAI doesn't:
- OpenAI explicitly provides device code + OAuth endpoints for third-party CLI tools (it's their Codex product strategy)
- Google's Gemini CLI OAuth uses client credentials that may violate ToS when used by third-party apps (the client ID belongs to Google's own Gemini CLI app, not to OpenClaw)
- Google's automated abuse detection flags unusual OAuth patterns from non-Google apps

**Recommendation**: Use `GEMINI_API_KEY` for Google. Use OAuth for OpenAI (safe, officially supported).

---

## OpenRouter

- Set `OPENROUTER_API_KEY` env var
- No OAuth — API key only
- Routes to any provider (OpenAI, Anthropic, Google, DeepSeek, etc.)
- Normalizes all responses to OpenAI Completions format
- CClaw's current default and simplest path

---

## DeepSeek (direct)

- Set `DEEPSEEK_API_KEY` env var
- API key only, no OAuth
- Base URL: `https://api.deepseek.com/v1`
- Cheapest option for DeepSeek models (no OpenRouter markup)
- China-based: data subject to Chinese data laws
- Use for non-sensitive work; US/EU provider for sensitive data

---

## CClaw Relevance

CClaw uses API keys exclusively (via env vars or config.json). OAuth flows are complex (browser callbacks, token refresh, device polling) and not needed when:
- OpenRouter handles all routing with one key
- Direct provider keys are simple to set up

If CClaw ever needs OAuth (e.g., letting users auth with ChatGPT subscription on a shared instance), the Device Code flow is the right choice — it works headless, no browser callback needed, just show a code and URL. Could be implemented as a JS extension.
