#ifndef CCLAW_LLM_TRANSPORT_H
#define CCLAW_LLM_TRANSPORT_H

/* Assembles the outbound LLM API request — URL, auth header, and body
 * shape — for the OpenAI, Gemini and Anthropic endpoint families. Pure request
 * construction; the transfer itself runs through http/llm_proc.
 */

#include "config.h"
#include "http.h"
#include <curl/curl.h>

/* Build LLM API URL from config (Gemini or OpenAI). Caller frees. */
char *llm_build_url(const Config *cfg);

/* Build auth header string. Caller frees. */
char *llm_build_auth_header(const Config *cfg);

/* The complete, NULL-terminated header list for one request: Content-Type,
 * `auth`, the caller's own `extra` header (or NULL), then whatever fixed
 * headers the wire format demands. Borrows every pointer. Returns the count. */
#define LLM_HEADERS_MAX 6
int llm_build_headers(const Config *cfg, const char *auth, const char *extra,
                      const char *out[LLM_HEADERS_MAX]);

/* Check if error body indicates context length exceeded. */
int llm_is_context_overflow(const char *body);

#endif
