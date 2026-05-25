#define _POSIX_C_SOURCE 200809L
#include "js_http_fetch.h"
#include "http_policy.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define FETCH_MAX (512 * 1024)

typedef struct { char *data; size_t len; size_t cap; } FetchBuf;

static size_t fetch_write_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    size_t bytes = size * nmemb;
    FetchBuf *buf = (FetchBuf *)ud;
    if (buf->len + bytes > FETCH_MAX) return 0;
    if (buf->len + bytes + 1 > buf->cap) {
        size_t nc = buf->cap ? buf->cap : 4096;
        while (nc < buf->len + bytes + 1) nc *= 2;
        char *tmp = realloc(buf->data, nc);
        if (!tmp) return 0;
        buf->data = tmp;
        buf->cap = nc;
    }
    memcpy(buf->data + buf->len, ptr, bytes);
    buf->len += bytes;
    buf->data[buf->len] = '\0';
    return bytes;
}

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

    /* Perform HTTP request */
    CURL *curl = curl_easy_init();
    if (!curl) { r.error = strdup("curl init failed"); return r; }

    FetchBuf resp = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fetch_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cclaw/1.0");

    if (!method) method = "GET";
    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_body ? req_body : "");
    } else if (strcmp(method, "PUT") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
        if (req_body) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_body);
    } else if (strcmp(method, "DELETE") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    }

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    if (rc == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        free(resp.data);
        r.error = strdup(curl_easy_strerror(rc));
        return r;
    }

    r.status = (int)status;
    r.body = resp.data;
    r.body_len = resp.len;
    return r;
}

void js_http_result_free(JsHttpResult *r) {
    free(r->body);
    free(r->error);
    r->body = NULL;
    r->error = NULL;
}
