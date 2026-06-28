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
    /* Timing metrics (populated on success) */
    double ttfb;            /* CURLINFO_STARTTRANSFER_TIME — first byte received */
    double tls_time;        /* CURLINFO_APPCONNECT_TIME — TLS handshake */
    int conn_reused;        /* 1 if CURLINFO_NUM_CONNECTS == 0 */
} HttpResponse;

/* Streaming upload read callback */
typedef size_t (*HttpReadFn)(char *dest, size_t size, size_t nmemb, void *userdata);

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
    const char *user_agent;      /* NULL = no User-Agent header */
    void *curl_handle;           /* If non-NULL, reuse this CURL* (caller owns lifecycle) */
} HttpRequestOpts;

/* General-purpose HTTP request. Returns HTTP status, -1 on curl error, -2 on timeout. */
int http_do(const HttpRequestOpts *opts, HttpResponse *resp);

/* POST JSON body to url with given headers (NULL-terminated array).
 * Returns HTTP status code, or -1 on curl error. */
int http_post(const char *url, const char **headers, const char *body,
              HttpResponse *resp);

/* Free response buffer. */
void http_response_free(HttpResponse *resp);

#endif
