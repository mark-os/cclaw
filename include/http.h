#ifndef CCLAW_HTTP_H
#define CCLAW_HTTP_H

#include <stddef.h>

/* Growable response buffer */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
    int retry_after;    /* Retry-After header value in seconds, 0 if absent */
} HttpResponse;

/* POST JSON body to url with given headers (NULL-terminated array of "Key: Value" strings).
 * Returns HTTP status code, or -1 on curl error.
 * Response body written to *resp (caller must call http_response_free). */
int http_post(const char *url, const char **headers, const char *body,
              HttpResponse *resp);

/* Free response buffer. */
void http_response_free(HttpResponse *resp);

#endif
