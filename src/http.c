#define _POSIX_C_SOURCE 200809L
#include "http.h"
#include "buf.h"
#include "log.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* Strip leading whitespace (provider keep-alive newlines) from response in-place */
static void resp_strip_leading_ws(HttpResponse *resp) {
    if (!resp->data || resp->len == 0) return;
    size_t skip = 0;
    while (skip < resp->len && (resp->data[skip] == '\n' || resp->data[skip] == '\r'
                                || resp->data[skip] == ' ' || resp->data[skip] == '\t'))
        skip++;
    if (skip == 0) return;
    resp->len -= skip;
    memmove(resp->data, resp->data + skip, resp->len + 1);
}

/* Context passed to curl write callback — keeps the Buf and size cap together */
typedef struct {
    Buf *buf;
    size_t max_bytes;
} WriteCbCtx;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t bytes = size * nmemb;
    WriteCbCtx *ctx = userdata;
    Buf *b = ctx->buf;

    if (ctx->max_bytes > 0 && b->len + bytes > ctx->max_bytes)
        return 0;

    buf_append(b, (const char *)ptr, bytes);
    return b->oom ? 0 : bytes;
}

/* V2: capture Retry-After and Content-Type headers */
static size_t header_cb(char *buf, size_t size, size_t nmemb, void *userdata) {
    size_t bytes = size * nmemb;
    HttpResponse *resp = userdata;

    /* Case-insensitive header matching */
    if (bytes > 12) {
        const char *prefix = "retry-after:";
        int match = 1;
        for (size_t i = 0; i < 12; i++) {
            if (tolower((unsigned char)buf[i]) != prefix[i]) { match = 0; break; }
        }
        if (match) {
            const char *val = buf + 12;
            while (*val == ' ') val++;
            resp->retry_after = atoi(val);
            if (resp->retry_after < 1) resp->retry_after = 1;
        }
    }
    if (bytes > 13) {
        const char *prefix = "content-type:";
        int match = 1;
        for (size_t i = 0; i < 13; i++) {
            if (tolower((unsigned char)buf[i]) != prefix[i]) { match = 0; break; }
        }
        if (match) {
            const char *val = buf + 13;
            while (*val == ' ') val++;
            size_t vlen = bytes - (size_t)(val - buf);
            while (vlen > 0 && (val[vlen-1] == '\r' || val[vlen-1] == '\n')) vlen--;
            if (vlen >= sizeof(resp->content_type)) vlen = sizeof(resp->content_type) - 1;
            memcpy(resp->content_type, val, vlen);
            resp->content_type[vlen] = '\0';
        }
    }
    return bytes;
}

/* General-purpose HTTP request */
int http_do(const HttpRequestOpts *opts, HttpResponse *resp) {
    memset(resp, 0, sizeof(*resp));
    if (!opts || !opts->url) return -1;

    Buf body = {0};
    WriteCbCtx wctx = { .buf = &body, .max_bytes = opts->max_response_bytes };

    int own_curl = 0;
    CURL *curl = opts->curl_handle;
    if (!curl) {
        curl = curl_easy_init();
        own_curl = 1;
    } else {
        curl_easy_reset(curl);
    }
    if (!curl) return -1;

    char errbuf[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_URL, opts->url);

    /* Method */
    int is_post = (opts->body || opts->read_cb);
    const char *method = opts->method;
    if (!method) method = is_post ? "POST" : "GET";

    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
    } else if (strcmp(method, "GET") != 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    }

    /* Body */
    if (opts->body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, opts->body);
    } else if (opts->read_cb) {
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, opts->read_cb);
        curl_easy_setopt(curl, CURLOPT_READDATA, opts->read_data);
    }

    /* Headers */
    struct curl_slist *hlist = NULL;
    if (opts->headers) {
        for (const char **h = opts->headers; *h; h++)
            hlist = curl_slist_append(hlist, *h);
    }
    if (opts->read_cb)
        hlist = curl_slist_append(hlist, "Transfer-Encoding: chunked");

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);

    /* Timeout */
    long timeout = opts->timeout > 0 ? opts->timeout : 300L;
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 10L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 90L);

    /* Redirects */
    if (opts->follow_redirects) {
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, opts->max_redirects > 0 ? (long)opts->max_redirects : 5L);
    }

    /* User agent */
    if (opts->user_agent)
        curl_easy_setopt(curl, CURLOPT_USERAGENT, opts->user_agent);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wctx);

    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, resp);

    CURLcode rc = curl_easy_perform(curl);

    long status = -1;
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        /* Task 6: connection reuse metrics (visible at TRACE level via caller) */
        long num_connects = 0;
        curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &num_connects);
        resp->conn_reused = (num_connects == 0);
        curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME, &resp->ttfb);
        curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME, &resp->tls_time);
    } else if (rc == CURLE_OPERATION_TIMEDOUT)
        status = -2;

    /* Stash curl error */
    if (rc != CURLE_OK && resp->err_detail[0] == '\0') {
        const char *msg = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        snprintf(resp->err_detail, sizeof(resp->err_detail), "%s", msg);
        cclaw_log_write(LOG_DEBUG, "http_do fail curl_code=%d err=%s", (int)rc, msg);
    }

    curl_slist_free_all(hlist);
    if (own_curl) curl_easy_cleanup(curl);

    /* Move accumulated body into resp's caller-visible fields. A transfer
     * that produced no bytes keeps data NULL (not "") so callers can fall
     * back to err_detail; OOM mid-body likewise surfaces as NULL. */
    if (body.len > 0) {
        resp->len = body.len;
        resp->data = buf_take(&body);
        if (!resp->data) resp->len = 0;
    } else {
        buf_free(&body);
    }

    resp_strip_leading_ws(resp);

    return (int)status;
}

int http_post(const char *url, const char **headers, const char *body,
              HttpResponse *resp) {
    HttpRequestOpts opts = {
        .url = url,
        .method = "POST",
        .body = body,
        .headers = headers,
    };
    return http_do(&opts, resp);
}


void http_response_free(HttpResponse *resp) {
    free(resp->data);
    resp->data = NULL;
    resp->len = 0;
}
