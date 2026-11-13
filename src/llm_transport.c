#define _POSIX_C_SOURCE 200809L
#include "llm_transport.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *llm_build_url(const Config *cfg) {
    const char *base = cfg->provider.base_url;
    size_t blen = strlen(base);
    if (blen > 0 && base[blen - 1] == '/') blen--;

    if (cfg->provider.endpoint_type == ENDPOINT_GEMINI) {
        const char *model = cfg->provider.model ? cfg->provider.model : "gemini-2.5-flash";
        size_t mlen = strlen(model);
        const char *action = "generateContent";
        size_t alen = strlen(action);
        size_t need = blen + 8 + mlen + 1 + alen + 1;
        char *url = malloc(need);
        if (!url) return NULL;
        snprintf(url, need, "%.*s/models/%s:%s", (int)blen, base, model, action);
        return url;
    }

    const char *path = cfg->provider.endpoint_type == ENDPOINT_ANTHROPIC
                           ? "/messages" : "/chat/completions";
    size_t plen = strlen(path);
    char *url = malloc(blen + plen + 1);
    if (!url) return NULL;
    memcpy(url, base, blen);
    memcpy(url + blen, path, plen + 1);
    return url;
}

char *llm_build_auth_header(const Config *cfg) {
    const char *key = cfg->provider.api_key ? cfg->provider.api_key : "";
    const char *fmt = cfg->provider.endpoint_type == ENDPOINT_GEMINI    ? "x-goog-api-key: %s"
                    : cfg->provider.endpoint_type == ENDPOINT_ANTHROPIC ? "x-api-key: %s"
                    : "Authorization: Bearer %s";
    size_t cap = strlen(fmt) + strlen(key) + 1;
    char *h = malloc(cap);
    if (!h) return NULL;
    snprintf(h, cap, fmt, key);
    return h;
}

int llm_build_headers(const Config *cfg, const char *auth, const char *extra,
                      const char *out[LLM_HEADERS_MAX]) {
    int n = 0;
    out[n++] = "Content-Type: application/json";
    out[n++] = auth;
    if (extra) out[n++] = extra;
    if (cfg->provider.endpoint_type == ENDPOINT_ANTHROPIC) {
        /* The version pins the request/response shape this build was written
         * against. The beta lets the body set thinking.block_binding (see
         * SQL_ANTHROPIC_FULL in llm_payload.c) and makes every response report
         * dropped thinking blocks in input_transformations. */
        out[n++] = "anthropic-version: 2026-10-21";
        out[n++] = "anthropic-beta: thinking-binding-controls-2026-08-01";
    }
    out[n] = NULL;
    return n;
}

int llm_is_context_overflow(const char *body) {
    if (!body) return 0;
    return (strstr(body, "context_length_exceeded") != NULL ||
            strstr(body, "maximum context length") != NULL ||
            strstr(body, "too many tokens") != NULL ||
            strstr(body, "prompt is too long") != NULL ||   /* Anthropic */
            strstr(body, "context window") != NULL);
}
