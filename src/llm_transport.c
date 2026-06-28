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

    const char *path = "/chat/completions";
    size_t plen = strlen(path);
    char *url = malloc(blen + plen + 1);
    if (!url) return NULL;
    memcpy(url, base, blen);
    memcpy(url + blen, path, plen + 1);
    return url;
}

char *llm_build_auth_header(const Config *cfg) {
    size_t key_len = cfg->provider.api_key ? strlen(cfg->provider.api_key) : 0;
    if (cfg->provider.endpoint_type == ENDPOINT_GEMINI) {
        size_t cap = 16 + key_len + 1;
        char *h = malloc(cap);
        if (!h) return NULL;
        snprintf(h, cap, "x-goog-api-key: %s", cfg->provider.api_key ? cfg->provider.api_key : "");
        return h;
    }
    size_t cap = 22 + key_len + 1;
    char *h = malloc(cap);
    if (!h) return NULL;
    snprintf(h, cap, "Authorization: Bearer %s", cfg->provider.api_key ? cfg->provider.api_key : "");
    return h;
}

int llm_is_context_overflow(const char *body) {
    if (!body) return 0;
    return (strstr(body, "context_length_exceeded") != NULL ||
            strstr(body, "maximum context length") != NULL ||
            strstr(body, "too many tokens") != NULL ||
            strstr(body, "context window") != NULL);
}
