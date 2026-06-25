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

/* V38: Execute HTTP fetch with allowed_hosts + SSRF protection.
 * Returns result struct. Caller must call js_http_result_free(). */
JsHttpResult js_http_fetch_exec(const char *url, const char *method,
                                const char *body,
                                char **allowed_hosts, size_t hosts_count);

void js_http_result_free(JsHttpResult *r);

#endif
