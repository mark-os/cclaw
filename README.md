# CClaw

A minimal AI agent in C. Runs on a Pogoplug V4 (ARMv5TE, 128MB RAM) as a Telegram bot or CLI REPL.

OpenAI-compatible LLM endpoints (OpenRouter, NVIDIA NIM, etc.) → tool-calling agent loop → shell_exec, file_read, file_write.

## Building

```bash
make          # native build
make test     # 18 tests
```

Cross-compile for Pogoplug: see [AGENTS.md](AGENTS.md#cross-compile-for-pogoplug-armv5te).

## Running

```bash
# Minimal: just needs an API key
export OPENROUTER_API_KEY="sk-or-v1-..."
./build/cclaw

# Or with config file
cat > config.json << 'EOF'
{
  "model": "deepseek/deepseek-v4-flash",
  "system_prompt": "You are a helpful assistant."
}
EOF
./build/cclaw config.json
```

With a `telegram_token` in config, it runs as a Telegram bot daemon instead of CLI.

## Architecture

~1700 lines of C. Session history on heap, per-turn arena for scratch. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Dependencies

- **libcurl** (dynamic, from system)
- **cJSON 1.7.19** (vendored)
- **SQLite 3.53.1** (vendored)
