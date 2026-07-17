#ifndef CCLAW_LLM_TRANSPORT_H
#define CCLAW_LLM_TRANSPORT_H

/* Assembles the outbound LLM API request — URL, auth header, and body
 * shape — for both the OpenAI and Gemini endpoint families. Pure request
 * construction; the transfer itself runs through http/llm_proc.
 */

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
