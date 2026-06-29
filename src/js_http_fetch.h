#ifndef CCLAW_JS_HTTP_FETCH_H
#define CCLAW_JS_HTTP_FETCH_H

#include <stddef.h>

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
JsHttpResult js_http_fetch_exec(const char *url, const char *method,
                                const char *body);

void js_http_result_free(JsHttpResult *r);

#endif
