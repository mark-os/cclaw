#ifndef CCLAW_LLM_H
#define CCLAW_LLM_H

#include "types.h"
#include "arena.h"

/* Tool schema for inclusion in LLM request */
typedef struct {
    const char *name;
    const char *description;
    const char *parameters_json;  /* raw JSON schema string */
} ToolSchema;

/* Token usage from LLM response */
typedef struct {
    int prompt_tokens;
    int completion_tokens;
    int total_tokens;
    int cache_read_tokens;   /* prompt cache hits (OpenAI/Anthropic/DeepSeek) */
    int cache_write_tokens;  /* prompt cache writes */
    int reasoning_tokens;    /* thinking/reasoning tokens (subset of completion) */
    int64_t cost_nano;       /* cost in nanodollars (10^-9 USD), 0 if not reported */
} LlmUsage;

/* Parsed LLM response */
typedef struct {
    char *content;          /* arena-allocated, NULL if tool_calls only */
    ToolCall *tool_calls;   /* arena-allocated array, NULL if none */
    size_t tool_call_count;
    LlmUsage usage;
    char *finish_reason;    /* arena-allocated */
    char *reasoning;        /* arena-allocated thinking/reasoning text, NULL if absent */
    char *logprobs_json;    /* arena-allocated raw logprobs JSON, NULL if absent */
} LlmResponse;

/* Parse OpenAI-compatible chat completions response JSON.
 * All strings/arrays in result are arena-allocated.
 * Returns 0 on success, -1 on parse failure. */
int llm_parse_response(Arena *a, const char *json, LlmResponse *out);

/* V35: normalize provider finish_reason string → StopReason enum.
 * Sole normalization point. NULL input → STOP_REASON_STOP. */
StopReason map_stop_reason(const char *finish_reason);

/* T290: Parse native Gemini generateContent response.
 * Returns 0 on success, -1 on parse failure. */
int llm_parse_response_gemini(Arena *a, const char *json, LlmResponse *out);

/* Populate LlmResponse directly from accumulated SSE context (no JSON round-trip). */
struct SseCtx;
int llm_response_from_sse(Arena *a, const struct SseCtx *ctx, LlmResponse *out);

#endif
