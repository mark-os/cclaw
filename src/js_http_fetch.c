#define _POSIX_C_SOURCE 200809L
#include "js_http_fetch.h"
#include "http.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

int js_http_body_is_binary(const char *body, size_t len) {
    return body && len && memchr(body, 0, len) != NULL;
}

JsHttpResult js_http_fetch_exec(const char *url, const char *method,
                                const char *req_body, const char **headers,
                                int timeout_secs, const char *save_to,
                                size_t max_bytes) {
    JsHttpResult r = {.status = -1, .body = NULL, .body_len = 0, .error = NULL};

    /* Scheme guard only — egress (host/IP/redirect gating) is enforced per-hop
     * by the broker proxy's decide(), reached via HTTP_PROXY. */
    if (!url || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
        r.error = strdup("url must start with http:// or https://");
        return r;
    }

    if (!method) method = "GET";

    FILE *sink = NULL;
    if (save_to) {
        if (max_bytes == 0) max_bytes = JS_HTTP_SAVE_MAX_DEFAULT;
        if (max_bytes > JS_HTTP_SAVE_MAX_CEILING) max_bytes = JS_HTTP_SAVE_MAX_CEILING;
        sink = fopen(save_to, "wb");
        if (!sink) {
            char msg[384];
            snprintf(msg, sizeof(msg), "save_to: cannot create %.200s: %s",
                     save_to, strerror(errno));
            r.error = strdup(msg);
            return r;
        }
    }

    HttpRequestOpts opts = {
        .url = url,
        .method = method,
        .body = (strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0) ? (req_body ? req_body : "") : NULL,
        .headers = headers,
        .timeout = (timeout_secs > 0 && timeout_secs <= JS_HTTP_TIMEOUT_MAX)
                   ? timeout_secs : JS_HTTP_TIMEOUT_DEFAULT,
        .follow_redirects = 1,
        .max_redirects = 5,
        .max_response_bytes = sink ? max_bytes : JS_HTTP_BODY_MAX,
        .user_agent = "cclaw/1.0",
        .sink = sink,
    };

    HttpResponse resp = {0};
    int status = http_do(&opts, &resp);

    /* The file is kept only for a complete 2xx body; anything else would be
     * a partial or empty file that looks like a download. */
    int kept = 0;
    if (sink) {
        int werr = (fclose(sink) != 0);
        kept = status >= 200 && status < 300 && !resp.truncated && !werr;
        if (!kept) unlink(save_to);
        if (status >= 200 && status < 300 && !kept) {
            char msg[160];
            if (resp.truncated)
                snprintf(msg, sizeof(msg), "response is larger than max_bytes "
                         "(%zu) — pass a larger max_bytes", max_bytes);
            else
                snprintf(msg, sizeof(msg), "save_to: write failed");
            r.error = strdup(msg);
            http_response_free(&resp);
            return r;
        }
    }

    if (status == -1 || status == -2) {
        r.error = strdup(resp.err_detail[0] ? resp.err_detail : "HTTP request failed");
        http_response_free(&resp);
        return r;
    }
    r.saved = kept;
    r.saved_bytes = resp.sink_bytes;

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
