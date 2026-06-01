#include "http.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
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

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t bytes = size * nmemb;
    HttpResponse *resp = userdata;

    if (resp->len + bytes + 1 > resp->cap) {
        size_t new_cap = (resp->cap == 0) ? 4096 : resp->cap;
        while (new_cap < resp->len + bytes + 1)
            new_cap *= 2;
        char *tmp = realloc(resp->data, new_cap);
        if (!tmp) return 0;
        resp->data = tmp;
        resp->cap = new_cap;
    }

    memcpy(resp->data + resp->len, ptr, bytes);
    resp->len += bytes;
    resp->data[resp->len] = '\0';
    return bytes;
}

/* V2: capture Retry-After header */
static size_t header_cb(char *buf, size_t size, size_t nmemb, void *userdata) {
    size_t bytes = size * nmemb;
    HttpResponse *resp = userdata;
    const char *prefix = "retry-after:";
    size_t plen = 12;
    if (bytes > plen) {
        /* Case-insensitive check */
        int match = 1;
        for (size_t i = 0; i < plen; i++) {
            if (tolower((unsigned char)buf[i]) != prefix[i]) { match = 0; break; }
        }
        if (match) {
            const char *val = buf + plen;
            while (*val == ' ') val++;
            resp->retry_after = atoi(val);
            if (resp->retry_after < 1) resp->retry_after = 1;
        }
    }
    return bytes;
}

int http_post(const char *url, const char **headers, const char *body,
              HttpResponse *resp) {
    memset(resp, 0, sizeof(*resp));

    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    struct curl_slist *hlist = NULL;
    if (headers) {
        for (const char **h = headers; *h; h++)
            hlist = curl_slist_append(hlist, *h);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 10L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 90L);

    CURLcode rc = curl_easy_perform(curl);

    long status = -1;
    if (rc == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    else if (rc == CURLE_OPERATION_TIMEDOUT)
        status = -2;

    curl_slist_free_all(hlist);
    curl_easy_cleanup(curl);

    resp_strip_leading_ws(resp);
    return (int)status;
}

void http_response_free(HttpResponse *resp) {
    free(resp->data);
    resp->data = NULL;
    resp->len = 0;
    resp->cap = 0;
}

int http_post_stream(const char *url, const char **headers,
                     HttpReadFn read_cb, void *read_data,
                     HttpResponse *resp) {
    memset(resp, 0, sizeof(*resp));

    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    struct curl_slist *hlist = NULL;
    if (headers) {
        for (const char **h = headers; *h; h++)
            hlist = curl_slist_append(hlist, *h);
    }
    /* Chunked transfer since content-length unknown */
    hlist = curl_slist_append(hlist, "Transfer-Encoding: chunked");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_cb);
    curl_easy_setopt(curl, CURLOPT_READDATA, read_data);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hlist);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 10L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 90L);

    CURLcode rc = curl_easy_perform(curl);

    long status = -1;
    if (rc == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    else if (rc == CURLE_OPERATION_TIMEDOUT)
        status = -2;

    curl_slist_free_all(hlist);
    curl_easy_cleanup(curl);

    resp_strip_leading_ws(resp);
    return (int)status;
}
