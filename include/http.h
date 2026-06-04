#ifndef CCLAW_HTTP_H
#define CCLAW_HTTP_H

#include <stddef.h>

/* Growable response buffer */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
    int retry_after;    /* Retry-After header value in seconds, 0 if absent */
    char err_detail[256]; /* curl error description on failure */
} HttpResponse;

/* POST JSON body to url with given headers (NULL-terminated array of "Key: Value" strings).
 * Returns HTTP status code, or -1 on curl error.
 * Response body written to *resp (caller must call http_response_free). */
int http_post(const char *url, const char **headers, const char *body,
              HttpResponse *resp);

/* V41: POST with streaming upload via CURLOPT_READFUNCTION.
 * read_cb/read_data provide the request body on demand (chunked transfer).
 * Returns HTTP status code, or -1 on curl error. */
typedef size_t (*HttpReadFn)(char *dest, size_t size, size_t nmemb, void *userdata);
int http_post_stream(const char *url, const char **headers,
                     HttpReadFn read_cb, void *read_data,
                     HttpResponse *resp);

/* SSE streaming: called with each content delta token.
 * Return 0 to continue, non-zero to abort. */
typedef int (*HttpSseFn)(const char *token, size_t len, void *userdata);

/* POST with streaming upload (READFUNCTION) + SSE response parsing.
 * On each SSE "data:" line, parses JSON delta and invokes sse_cb with content.
 * Full accumulated response stored in resp->data for final parsing.
 * Returns HTTP status code, or -1 on curl error. */
int http_post_stream_sse(const char *url, const char **headers,
                         HttpReadFn read_cb, void *read_data,
                         HttpSseFn sse_cb, void *sse_data,
                         HttpResponse *resp);

/* Free response buffer. */
void http_response_free(HttpResponse *resp);

#endif
