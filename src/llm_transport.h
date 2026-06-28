#ifndef CCLAW_LLM_TRANSPORT_H
#define CCLAW_LLM_TRANSPORT_H

#include "config.h"
#include "http.h"
#include <curl/curl.h>

/* Build LLM API URL from config (Gemini or OpenAI). Caller frees. */
char *llm_build_url(const Config *cfg);

/* Build auth header string. Caller frees. */
char *llm_build_auth_header(const Config *cfg);

/* Check if error body indicates context length exceeded. */
int llm_is_context_overflow(const char *body);

#endif
