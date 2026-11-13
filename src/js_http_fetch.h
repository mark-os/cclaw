#ifndef CCLAW_JS_HTTP_FETCH_H
#define CCLAW_JS_HTTP_FETCH_H

/* The http_request() bridge for the JS tier: performs one bounded HTTP
 * fetch on behalf of sandboxed QuickJS, returning status + body to the
 * engine.
 */

#include <stddef.h>

/* Seconds. The default suits a normal API call; the ceiling exists because a
 * JS tool blocks its whole tool child while the fetch runs, so an unbounded
 * value would be a hang. Slow paths that legitimately need longer — an
 * LLM-backed search grounding a query, on a slow box — pass their own. */
#define JS_HTTP_TIMEOUT_DEFAULT 30
/* Ceiling a caller may raise the per-request timeout to — sized for the
 * slowest working setup, not the fastest (2026-08-27 timeout audit). */
#define JS_HTTP_TIMEOUT_MAX     600

/* Bytes. A body returned to JS is held in memory twice (C buffer, then the
 * heap-limited engine), so it has a fixed cap. A save_to download streams to
 * disk and never enters either, so its cap is only a guard against filling
 * the disk: a default the caller may raise, and a ceiling so it stays a cap. */
#define JS_HTTP_BODY_MAX         (2u * 1024 * 1024)
#define JS_HTTP_SAVE_MAX_DEFAULT (64u * 1024 * 1024)
#define JS_HTTP_SAVE_MAX_CEILING (1024u * 1024 * 1024)

/* Result from js_http_fetch_exec. Caller must free body. */
typedef struct {
    int status;       /* HTTP status code, or -1 on error */
    char *body;       /* response body (heap-allocated) */
    size_t body_len;
    char *error;      /* error message if status == -1 (heap-allocated) */
    int saved;        /* 1 = the body is in the save_to file, not in body */
    size_t saved_bytes;
} JsHttpResult;

/* Execute an HTTP fetch. Egress is enforced per-hop by the broker proxy
 * (HTTP_PROXY → net_shim → decide()), NOT a pre-flight allowlist — a single
 * check can't see redirects. Returns result struct; caller frees with
 * js_http_result_free(). */
/* headers: NULL, or a NULL-terminated array of "Name: Value" strings.
 * save_to: NULL, or a file to stream a 2xx body into (max_bytes caps it; 0 =
 * JS_HTTP_SAVE_MAX_DEFAULT). The file exists afterwards only when saved=1:
 * a non-2xx answer, a failed transfer and an over-cap body all remove it. */
JsHttpResult js_http_fetch_exec(const char *url, const char *method,
                                const char *body, const char **headers,
                                int timeout_secs, const char *save_to,
                                size_t max_bytes);

/* 1 when the bytes cannot be a text body: a NUL never occurs in text of any
 * encoding a script could use as a string, and every common binary format
 * (JPEG, PNG, PDF, zip, gzip) has one within its first bytes. */
int js_http_body_is_binary(const char *body, size_t len);

void js_http_result_free(JsHttpResult *r);

#endif
