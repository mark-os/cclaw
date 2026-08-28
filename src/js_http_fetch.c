#define _POSIX_C_SOURCE 200809L
#include "js_http_fetch.h"
#include "http.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

JsHttpResult js_http_fetch_exec(const char *url, const char *method,
                                const char *req_body, const char **headers,
                                int timeout_secs) {
    JsHttpResult r = {.status = -1, .body = NULL, .body_len = 0, .error = NULL};

    /* Scheme guard only — egress (host/IP/redirect gating) is enforced per-hop
     * by the broker proxy's decide(), reached via HTTP_PROXY. */
    if (!url || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
        r.error = strdup("url must start with http:// or https://");
        return r;
    }

    if (!method) method = "GET";

    HttpRequestOpts opts = {
        .url = url,
        .method = method,
        .body = (strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0) ? (req_body ? req_body : "") : NULL,
        .headers = headers,
        .timeout = (timeout_secs > 0 && timeout_secs <= JS_HTTP_TIMEOUT_MAX)
                   ? timeout_secs : JS_HTTP_TIMEOUT_DEFAULT,
        .follow_redirects = 1,
        .max_redirects = 5,
        .max_response_bytes = 2 * 1024 * 1024,
        .user_agent = "cclaw/1.0",
    };

    HttpResponse resp = {0};
    int status = http_do(&opts, &resp);

    if (status == -1 || status == -2) {
        r.error = strdup(resp.err_detail[0] ? resp.err_detail : "HTTP request failed");
        http_response_free(&resp);
        return r;
    }

    r.status = status;
    r.body = resp.data;
    r.body_len = resp.len;
    /* Transfer ownership — don't free resp.data */
    return r;
}

void js_http_result_free(JsHttpResult *r) {
    free(r->body);
    free(r->error);
    r->body = NULL;
    r->error = NULL;
}
