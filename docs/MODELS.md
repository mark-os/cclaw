# Model Selection & Provider Config

## Phase 1 Provider: OpenRouter

Primary provider for development and testing. OpenAI-compatible API, key already on EC2 machine.

- **Endpoint:** `https://openrouter.ai/api/v1/chat/completions`
- **Auth:** `Authorization: Bearer $OPENROUTER_API_KEY`
- **Format:** OpenAI-compatible
- **Env var:** `OPENROUTER_API_KEY` (73 chars, starts with `sk-or-v1-`)

### Primary Model: DeepSeek V4 Flash

- **Model ID:** `deepseek/deepseek-v4-flash`
- **Params:** 284B total, 13B activated (MoE)
- **Context:** 1M tokens
- **Pricing:** $0.14/$0.28 per M tokens (very cheap)
- **Reasoning:** Includes reasoning tokens (transparent chain-of-thought)
- **Tested:** Works from EC2, routed to Parasail

## Alternative Provider: NVIDIA NIM

Faster inference, no reasoning overhead on non-reasoning models.

- **Endpoint:** `https://integrate.api.nvidia.com/v1/chat/completions`
- **Auth:** `Authorization: Bearer $NVIDIA_API_KEY`
- **Env var:** `NVIDIA_API_KEY` (70 chars, starts with `nvapi-`)

### Recommended NVIDIA Models

| Model | Speed | Notes |
|-------|-------|-------|
| `mistralai/mistral-nemotron` | 486ms | Fastest, most reliable |
| `meta/llama-3.3-70b-instruct` | 465ms | Strong general purpose |
| `deepseek-ai/deepseek-v3.1-terminus` | 757ms | Good at code |
| `qwen/qwen3-next-80b-a3b-instruct` | 516ms | Fast MoE |

## Env Var Convention

CClaw reads standard provider env vars directly — no `CCLAW_` prefix:

```bash
# Phase 1 (OpenRouter)
export OPENROUTER_API_KEY="sk-or-v1-..."

# Alternative (NVIDIA NIM)
export NVIDIA_API_KEY="nvapi-..."
```

The code reads whichever is configured. Priority: check for a base_url override env var, otherwise default to OpenRouter.

```bash
# Optional overrides
export CCLAW_BASE_URL="https://integrate.api.nvidia.com/v1"  # switch provider
export CCLAW_MODEL="mistralai/mistral-nemotron"              # switch model
```

Defaults (no env vars needed beyond the API key):
- Base URL: `https://openrouter.ai/api/v1`
- Model: `deepseek/deepseek-v4-flash`
- API key: reads `OPENROUTER_API_KEY` (or `NVIDIA_API_KEY` if `CCLAW_BASE_URL` points to NVIDIA)

## Request Format

Standard OpenAI chat completions:
```json
{
  "model": "deepseek/deepseek-v4-flash",
  "messages": [...],
  "tools": [...],
  "max_tokens": 4096,
  "stream": false
}
```

## Response Format

```json
{
  "choices": [{
    "message": {
      "role": "assistant",
      "content": "...",
      "tool_calls": [{"id": "...", "type": "function", "function": {"name": "...", "arguments": "..."}}]
    },
    "finish_reason": "stop|tool_calls|length"
  }],
  "usage": {
    "prompt_tokens": 100,
    "completion_tokens": 50,
    "total_tokens": 150
  }
}
```

OpenRouter adds: `provider` field, `usage.cost`, `reasoning` field in message.

## Tool Call Format

```json
{
  "tools": [{
    "type": "function",
    "function": {
      "name": "shell_exec",
      "description": "Execute a shell command",
      "parameters": {
        "type": "object",
        "properties": {
          "command": {"type": "string", "description": "The command to execute"}
        },
        "required": ["command"]
      }
    }
  }]
}
```

Tool results:
```json
{
  "role": "tool",
  "tool_call_id": "call_abc123",
  "content": "command output here"
}
```

## Quick Test Commands

```bash
# OpenRouter (from EC2)
curl -sS --max-time 15 https://openrouter.ai/api/v1/chat/completions \
  -H "Authorization: Bearer $OPENROUTER_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{"model":"deepseek/deepseek-v4-flash","messages":[{"role":"user","content":"Say hello in one word."}],"max_tokens":64}'

# NVIDIA NIM
curl -sS --max-time 15 https://integrate.api.nvidia.com/v1/chat/completions \
  -H "Authorization: Bearer $NVIDIA_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{"model":"mistralai/mistral-nemotron","messages":[{"role":"user","content":"Say hello in one word."}],"max_tokens":64}'
```

## Privacy Notes

- OpenRouter routes to various backends — data may hit DeepSeek (China) for DeepSeek models
- NVIDIA NIM is US-based
- For sensitive work: use NVIDIA NIM or Together AI
- For CClaw dev/testing: OpenRouter is fine (not handling sensitive data)
