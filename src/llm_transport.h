#ifndef CCLAW_LLM_TRANSPORT_H
#define CCLAW_LLM_TRANSPORT_H

#include "arena.h"
#include "config.h"
#include "http.h"
#include <curl/curl.h>

/* Build LLM API URL from config (Gemini or OpenAI). Arena-allocated. */
char *llm_build_url(Arena *a, const Config *cfg);

/* Build auth header string. Arena-allocated. */
char *llm_build_auth_header(Arena *a, const Config *cfg);

/* Check if error body indicates context length exceeded. */
int llm_is_context_overflow(const char *body);

/* Default SSE callback: write tokens to stdout. */
int llm_sse_stdout_cb(const char *token, size_t len, void *userdata);

#endif
