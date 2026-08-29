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

/* Result from js_http_fetch_exec. Caller must free body. */
typedef struct {
    int status;       /* HTTP status code, or -1 on error */
    char *body;       /* response body (heap-allocated) */
    size_t body_len;
    char *error;      /* error message if status == -1 (heap-allocated) */
} JsHttpResult;

/* Execute an HTTP fetch. Egress is enforced per-hop by the broker proxy
 * (HTTP_PROXY → net_shim → decide()), NOT a pre-flight allowlist — a single
 * check can't see redirects. Returns result struct; caller frees with
 * js_http_result_free(). */
/* headers: NULL, or a NULL-terminated array of "Name: Value" strings. */
JsHttpResult js_http_fetch_exec(const char *url, const char *method,
                                const char *body, const char **headers,
                                int timeout_secs);

void js_http_result_free(JsHttpResult *r);

#endif
