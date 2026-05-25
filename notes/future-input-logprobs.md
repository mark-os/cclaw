# Future: Input Logprobs for Context Management

## Idea
Use input/prompt token logprobs to intelligently prune session history.
Low-probability input tokens = surprising/informative context (keep).
High-probability input tokens = predictable/redundant (safe to drop).

Better than naive token-counting for context window management.

## Prerequisite
Local model inference (vLLM with `--return-prompt-logprobs`, or similar).
Not available through OpenRouter or any hosted OpenAI-compatible API.

## When
After local model support is implemented (Ollama, vLLM, llama.cpp).
