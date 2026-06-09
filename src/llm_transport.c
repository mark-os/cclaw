#define _POSIX_C_SOURCE 200809L
#include "llm_transport.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_RETRIES 5
#define INITIAL_BACKOFF_MS 1000

int llm_sse_stdout_cb(const char *token, size_t len, void *userdata) {
    (void)userdata;
    if (token && len > 0)
        fwrite(token, 1, len, stdout);
    fflush(stdout);
    return 0;
}

char *llm_build_url(Arena *a, const Config *cfg) {
    const char *base = cfg->provider.base_url;
    size_t blen = strlen(base);
    if (blen > 0 && base[blen - 1] == '/') blen--;

    if (cfg->provider.endpoint_type == ENDPOINT_GEMINI) {
        const char *model = cfg->provider.model ? cfg->provider.model : "gemini-2.5-flash";
        size_t mlen = strlen(model);
        const char *action = cfg->stream ? "streamGenerateContent?alt=sse" : "generateContent";
        size_t alen = strlen(action);
        size_t need = blen + 8 + mlen + 1 + alen + 1;
        char *url = arena_alloc(a, need);
        if (!url) return NULL;
        snprintf(url, need, "%.*s/models/%s:%s", (int)blen, base, model, action);
        return url;
    }

    const char *path = "/chat/completions";
    size_t plen = strlen(path);
    char *url = arena_alloc(a, blen + plen + 1);
    if (!url) return NULL;
    memcpy(url, base, blen);
    memcpy(url + blen, path, plen + 1);
    return url;
}

char *llm_build_auth_header(Arena *a, const Config *cfg) {
    size_t key_len = cfg->provider.api_key ? strlen(cfg->provider.api_key) : 0;
    if (cfg->provider.endpoint_type == ENDPOINT_GEMINI) {
        char *h = arena_alloc(a, 16 + key_len + 1);
        if (!h) return NULL;
        sprintf(h, "x-goog-api-key: %s", cfg->provider.api_key ? cfg->provider.api_key : "");
        return h;
    }
    char *h = arena_alloc(a, 22 + key_len + 1);
    if (!h) return NULL;
    sprintf(h, "Authorization: Bearer %s", cfg->provider.api_key ? cfg->provider.api_key : "");
    return h;
}

int llm_call_with_retry(const char *url, const char **headers,
                        RequestStreamer *rs, HttpResponse *resp,
                        const Config *cfg, HttpSseFn sse_cb, void *sse_data,
                        SseCtx *out_ctx, CURL *curl) {
    int backoff_ms = INITIAL_BACKOFF_MS;
    int last_status = -1;

    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        int status;
        if (cfg->stream) {
            HttpRequestOpts opts = {
                .url = url, .method = "POST", .headers = headers,
                .read_cb = rs_read_cb, .read_data = rs,
                .sse_cb = sse_cb, .sse_data = sse_data,
                .out_ctx = out_ctx, .curl_handle = curl,
            };
            status = http_do(&opts, resp);
        } else {
            HttpRequestOpts opts = {
                .url = url, .method = "POST", .headers = headers,
                .read_cb = rs_read_cb, .read_data = rs,
                .curl_handle = curl,
            };
            status = http_do(&opts, resp);
        }
        last_status = status;

        if (status == -1 || status == -2) return status;
        if (status == 401 || status == 403 || status == 404) return status;

        if (status == 429 || (status >= 500 && status < 600)) {
            int wait_sec = resp->retry_after > 0 ? resp->retry_after : (backoff_ms / 1000);
            if (wait_sec < 1) wait_sec = 1;
            LOG_DEBUG_(cfg, "HTTP %d, retry %d/%d (wait %ds)",
                      status, attempt + 1, MAX_RETRIES, wait_sec);
            sleep((unsigned)wait_sec);
            http_response_free(resp);
            if (out_ctx) { sse_ctx_free(out_ctx); memset(out_ctx, 0, sizeof(*out_ctx)); }
            rs_reset(rs);
            backoff_ms *= 2;
            continue;
        }
        return status;
    }
    return last_status;
}

int llm_is_context_overflow(const char *body) {
    if (!body) return 0;
    return (strstr(body, "context_length_exceeded") != NULL ||
            strstr(body, "maximum context length") != NULL ||
            strstr(body, "too many tokens") != NULL ||
            strstr(body, "context window") != NULL);
}

static void trace_dump_request(const Config *cfg, sqlite3 *db, int64_t session_id,
                               const Config *rs_cfg, const ContextPlan *plan,
                               const ToolSchema *tools, size_t tool_count,
                               const char *recall_text, const char *gemini_cache_name,
                               const char *label) {
    RequestStreamer dbg_rs;
    if (rs_init(&dbg_rs, db, session_id, rs_cfg, plan, tools, tool_count) != 0) return;
    dbg_rs.recall_text = recall_text;
    dbg_rs.gemini_cache_name = gemini_cache_name;
    size_t cap = 4096, len = 0;
    char *dbuf = malloc(cap);
    if (dbuf) {
        while (1) {
            if (len + 1024 > cap) { cap *= 2; dbuf = realloc(dbuf, cap); if (!dbuf) break; }
            size_t n = rs_read_cb(dbuf + len, 1, 1024, &dbg_rs);
            if (n == 0) break;
            len += n;
        }
        if (dbuf) { dbuf[len] = '\0'; LOG_TRACE_(cfg, "REQ %s%s", label, dbuf); free(dbuf); }
    }
    rs_cleanup(&dbg_rs);
}

int llm_call_with_fallbacks(Arena *a, sqlite3 *db, int64_t session_id,
                            const Config *cfg, const ContextPlan *plan,
                            const ToolSchema *tools, size_t tool_count,
                            HttpResponse *resp, HttpSseFn sse_cb, void *sse_data,
                            SseCtx *out_ctx, const char *recall_text,
                            const char *gemini_cache_name,
                            const char *body_override, CURL *curl) {
    /* Mock mode */
    const char *mock_path = getenv("CCLAW_LLM_MOCK");
    if (mock_path) {
        FILE *f = fopen(mock_path, "r");
        if (!f) return -1;
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        resp->data = malloc((size_t)len + 1);
        if (resp->data) {
            fread(resp->data, 1, (size_t)len, f);
            resp->data[len] = '\0';
            resp->len = (size_t)len;
        }
        fclose(f);
        return resp->data ? 200 : -1;
    }

    char *url = llm_build_url(a, cfg);
    if (!url) return -1;

    char *auth_hdr = llm_build_auth_header(a, cfg);
    if (!auth_hdr) return -1;

    char *session_hdr = arena_alloc(a, 64);
    if (!session_hdr) return -1;
    snprintf(session_hdr, 64, "x-session-id: cclaw-%lld", (long long)session_id);
    const char *headers[] = { "Content-Type: application/json", auth_hdr, session_hdr, NULL };

    /* body_override path (hook-modified request) */
    if (body_override) {
        if (cfg->log_level >= LOG_LEVEL_TRACE)
            LOG_TRACE_(cfg, "REQ (hook-modified) %s", body_override);
        HttpRequestOpts opts = {
            .url = url, .method = "POST", .headers = headers,
            .body = body_override, .curl_handle = curl,
        };
        int status = http_do(&opts, resp);
        if (status >= 200 && status < 300) return status;
        if (status >= 300 && status < 500 && status != 401 && status != 403 &&
            status != 404 && status != 429)
            return status;
        /* Fallbacks with body_override */
        for (size_t i = 0; i < cfg->fallback_count; i++) {
            const ProviderConfig *fb = &cfg->fallback_providers[i];
            if (!fb->base_url || !fb->api_key || !fb->model) continue;
            size_t blen = strlen(fb->base_url);
            if (blen > 0 && fb->base_url[blen - 1] == '/') blen--;
            char *fb_url = arena_alloc(a, blen + 18);
            if (!fb_url) continue;
            memcpy(fb_url, fb->base_url, blen);
            memcpy(fb_url + blen, "/chat/completions", 18);
            size_t fb_key_len = strlen(fb->api_key);
            char *fb_auth = arena_alloc(a, 22 + fb_key_len + 1);
            if (!fb_auth) continue;
            sprintf(fb_auth, "Authorization: Bearer %s", fb->api_key);
            const char *fb_headers[] = { "Content-Type: application/json", fb_auth, session_hdr, NULL };
            http_response_free(resp);
            HttpRequestOpts fb_opts = {
                .url = fb_url, .method = "POST", .headers = fb_headers,
                .body = body_override, .curl_handle = curl,
            };
            status = http_do(&fb_opts, resp);
            if (status != -1) return status;
        }
        return status;
    }

    /* Streaming upload path */
    RequestStreamer rs;
    if (rs_init(&rs, db, session_id, cfg, plan, tools, tool_count) != 0)
        return -1;
    rs.recall_text = recall_text;
    rs.gemini_cache_name = gemini_cache_name;

    if (cfg->log_level >= LOG_LEVEL_TRACE)
        trace_dump_request(cfg, db, session_id, cfg, plan, tools, tool_count,
                           recall_text, gemini_cache_name, "");

    int status = llm_call_with_retry(url, headers, &rs, resp, cfg, sse_cb, sse_data, out_ctx, curl);
    rs_cleanup(&rs);

    if (status >= 200 && status < 300) return status;
    if (status >= 300 && status < 500 && status != 401 && status != 403 &&
        status != 404 && status != 429)
        return status;

    /* Fallback providers */
    for (size_t i = 0; i < cfg->fallback_count; i++) {
        LOG_DEBUG_(cfg, "primary failed, trying fallback %zu", i);
        const ProviderConfig *fb = &cfg->fallback_providers[i];
        if (!fb->base_url || !fb->api_key || !fb->model) continue;

        size_t blen = strlen(fb->base_url);
        if (blen > 0 && fb->base_url[blen - 1] == '/') blen--;
        char *fb_url = arena_alloc(a, blen + 18);
        if (!fb_url) continue;
        memcpy(fb_url, fb->base_url, blen);
        memcpy(fb_url + blen, "/chat/completions", 18);

        Config fb_cfg = *cfg;
        fb_cfg.provider = *fb;

        RequestStreamer fb_rs;
        if (rs_init(&fb_rs, db, session_id, &fb_cfg, plan, tools, tool_count) != 0) continue;
        fb_rs.recall_text = recall_text;

        if (cfg->log_level >= LOG_LEVEL_TRACE)
            trace_dump_request(cfg, db, session_id, &fb_cfg, plan, tools, tool_count,
                               recall_text, NULL, "fallback: ");

        size_t fb_key_len = strlen(fb->api_key);
        char *fb_auth = arena_alloc(a, 22 + fb_key_len + 1);
        if (!fb_auth) { rs_cleanup(&fb_rs); continue; }
        sprintf(fb_auth, "Authorization: Bearer %s", fb->api_key);
        const char *fb_headers[] = { "Content-Type: application/json", fb_auth, session_hdr, NULL };

        http_response_free(resp);
        if (out_ctx) { sse_ctx_free(out_ctx); memset(out_ctx, 0, sizeof(*out_ctx)); }
        /* Note: fallback uses same curl handle — different endpoint but same thread */
        status = llm_call_with_retry(fb_url, fb_headers, &fb_rs, resp, cfg, sse_cb, sse_data, out_ctx, curl);
        rs_cleanup(&fb_rs);
        if (status != -1) return status;
    }
    return status;
}
