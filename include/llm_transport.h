#ifndef CCLAW_LLM_TRANSPORT_H
#define CCLAW_LLM_TRANSPORT_H

#include "arena.h"
#include "config.h"
#include "context.h"
#include "http.h"
#include "llm.h"
#include "request_stream.h"
#include "sqlite3.h"
#include <curl/curl.h>

/* Build LLM API URL from config (Gemini or OpenAI). Arena-allocated. */
char *llm_build_url(Arena *a, const Config *cfg);

/* Build auth header string. Arena-allocated. */
char *llm_build_auth_header(Arena *a, const Config *cfg);

/* Call LLM with retry on 429/5xx. Resets streamer on retry.
 * curl: if non-NULL, reuse this handle (caller owns lifecycle).
 * Returns HTTP status, -1 on network error, -2 on timeout. */
int llm_call_with_retry(const char *url, const char **headers,
                        RequestStreamer *rs, HttpResponse *resp,
                        const Config *cfg, HttpSseFn sse_cb, void *sse_data,
                        SseCtx *out_ctx, CURL *curl);

/* Full LLM call: build URL/auth, init streamer, retry, fallback providers.
 * curl: if non-NULL, reuse this handle for connection persistence.
 * body_override: if non-NULL, POSTs this body instead of using streamer.
 * Returns HTTP status. */
int llm_call_with_fallbacks(Arena *a, sqlite3 *db, int64_t session_id,
                            const Config *cfg, const ContextPlan *plan,
                            const ToolSchema *tools, size_t tool_count,
                            HttpResponse *resp, HttpSseFn sse_cb, void *sse_data,
                            SseCtx *out_ctx, const char *recall_text,
                            const char *gemini_cache_name,
                            const char *body_override, CURL *curl);

/* Check if error body indicates context length exceeded. */
int llm_is_context_overflow(const char *body);

/* Default SSE callback: write tokens to stdout. */
int llm_sse_stdout_cb(const char *token, size_t len, void *userdata);

#endif
