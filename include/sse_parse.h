#ifndef CCLAW_SSE_PARSE_H
#define CCLAW_SSE_PARSE_H

#include <stddef.h>
#include <stdint.h>

/* Per-chunk parsed result from SSE JSON */
typedef struct {
    const char *text;       size_t text_len;
    const char *reasoning;  size_t reasoning_len;
    const char *tc_name;    size_t tc_name_len;
    const char *tc_id;      size_t tc_id_len;
    const char *tc_args;    size_t tc_args_len;
    int tc_index;           /* tool_call index (-1 if none) */
    int tc_args_complete;   /* 1 = full object (Gemini), 0 = fragment (OpenAI/Anthropic) */
    const char *finish;     size_t finish_len;
    int prompt_tokens;
    int completion_tokens;
    int total_tokens;
    int cache_read_tokens;
    int cache_write_tokens;
    int64_t cost_nano;
} SseChunk;

/* Parse one SSE data JSON chunk (OpenAI-compatible format).
 * json/len = the raw JSON after "data: " prefix.
 * Returns 0 on success, -1 on parse error. */
int sse_parse_openai(const char *json, size_t len, SseChunk *out);

/* Parse one SSE data JSON chunk (Gemini format).
 * Returns 0 on success, -1 on parse error. */
int sse_parse_gemini(const char *json, size_t len, SseChunk *out);

/* Parse one SSE event (Anthropic format).
 * event_type = the "event:" line value (e.g. "content_block_delta").
 * json/len = the "data:" JSON payload.
 * Returns 0 on success, -1 on parse error. */
int sse_parse_anthropic(const char *event_type, const char *json, size_t len, SseChunk *out);

#endif
