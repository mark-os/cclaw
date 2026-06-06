#define _POSIX_C_SOURCE 200809L
#include "js_http_fetch.h"
#include "http.h"
#include "http_policy.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

JsHttpResult js_http_fetch_exec(const char *url, const char *method,
                                const char *req_body,
                                char **allowed_hosts, size_t hosts_count) {
    JsHttpResult r = {.status = -1, .body = NULL, .body_len = 0, .error = NULL};

    /* V38: no allowlist = no network from JS */
    if (!allowed_hosts || hosts_count == 0) {
        r.error = strdup("no allowed_hosts configured");
        return r;
    }

    /* V46: use HttpPolicy layer for validation */
    HttpPolicy policy = {
        .allowed_hosts = allowed_hosts,
        .allowed_count = hosts_count,
        .blocked_hosts = NULL,
        .blocked_count = 0,
        .block_private = 1
    };

    char err[256];
    if (http_check_policy(url, &policy, err, sizeof(err)) != 0) {
        r.error = strdup(err);
        return r;
    }

    if (!method) method = "GET";

    HttpRequestOpts opts = {
        .url = url,
        .method = method,
        .body = (strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0) ? (req_body ? req_body : "") : NULL,
        .timeout = 30,
        .follow_redirects = 1,
        .max_redirects = 5,
        .max_response_bytes = 512 * 1024,
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
