#ifndef CCLAW_HTTP_H
#define CCLAW_HTTP_H

#include <stddef.h>
#include <stdint.h>

/* Growable response buffer */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
    size_t max_bytes;       /* 0 = unlimited, else abort write_cb when exceeded */
    int retry_after;        /* Retry-After header value in seconds, 0 if absent */
    char content_type[128]; /* Content-Type header value, empty if absent */
    char err_detail[256];   /* curl error description on failure */
} HttpResponse;

/* Streaming upload read callback */
typedef size_t (*HttpReadFn)(char *dest, size_t size, size_t nmemb, void *userdata);

/* SSE streaming: called with each content delta token.
 * Return 0 to continue, non-zero to abort. */
typedef int (*HttpSseFn)(const char *token, size_t len, void *userdata);

/* Accumulated SSE context — public so callers can extract fields directly */
typedef struct SseCtx {
    HttpResponse *resp;
    HttpSseFn sse_cb;
    void *sse_data;
    char *line_buf;
    size_t line_len;
    size_t line_cap;
    char *content;
    size_t content_len;
    size_t content_cap;
    char *reasoning;
    size_t reasoning_len;
    size_t reasoning_cap;
    int reasoning_started;
    char **tc_ids;
    char **tc_names;
    char **tc_args;
    size_t *tc_arg_lens;
    size_t *tc_arg_caps;
    size_t tc_count;
    size_t tc_alloc;
    char *finish_reason;
    int prompt_tokens;
    int completion_tokens;
    int total_tokens;
    int cache_read_tokens;
    int cache_write_tokens;
    int64_t cost_nano;
} SseCtx;

/* Free all heap fields in an SseCtx (does not free the struct itself). */
void sse_ctx_free(SseCtx *ctx);

/* General-purpose HTTP request options */
typedef struct {
    const char *url;
    const char *method;          /* GET, POST, PUT, DELETE (default: GET, or POST if body/read_cb set) */
    const char *body;            /* Static body (mutually exclusive with read_cb) */
    const char **headers;        /* NULL-terminated array, or NULL */
    long timeout;                /* Seconds, 0 = default 300 */
    int follow_redirects;        /* 1 = follow, 0 = don't */
    int max_redirects;           /* Max redirects if following (default 5) */
    size_t max_response_bytes;   /* 0 = unlimited */
    HttpReadFn read_cb;          /* Streaming upload (chunked transfer) */
    void *read_data;
    HttpSseFn sse_cb;            /* SSE response parsing callback */
    void *sse_data;
    SseCtx *out_ctx;             /* If set, SSE accumulation transferred here */
    const char *user_agent;      /* NULL = no User-Agent header */
    void *curl_handle;           /* If non-NULL, reuse this CURL* (caller owns lifecycle) */
} HttpRequestOpts;

/* General-purpose HTTP request. Returns HTTP status, -1 on curl error, -2 on timeout. */
int http_do(const HttpRequestOpts *opts, HttpResponse *resp);

/* POST JSON body to url with given headers (NULL-terminated array).
 * Returns HTTP status code, or -1 on curl error. */
int http_post(const char *url, const char **headers, const char *body,
              HttpResponse *resp);

/* POST with streaming upload via CURLOPT_READFUNCTION (chunked transfer).
 * Returns HTTP status code, or -1 on curl error. */
int http_post_stream(const char *url, const char **headers,
                     HttpReadFn read_cb, void *read_data,
                     HttpResponse *resp);

/* POST with streaming upload + SSE response parsing.
 * If out_ctx is non-NULL, accumulated SSE state is transferred there (no JSON reconstruction).
 * Returns HTTP status code, or -1 on curl error. */
int http_post_stream_sse(const char *url, const char **headers,
                         HttpReadFn read_cb, void *read_data,
                         HttpSseFn sse_cb, void *sse_data,
                         HttpResponse *resp, SseCtx *out_ctx);

/* Free response buffer. */
void http_response_free(HttpResponse *resp);

#endif
