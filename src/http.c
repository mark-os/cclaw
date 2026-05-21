#include "http.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

    CURLcode rc = curl_easy_perform(curl);

    long status = -1;
    if (rc == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    curl_slist_free_all(hlist);
    curl_easy_cleanup(curl);

    return (rc == CURLE_OK) ? (int)status : -1;
}

void http_response_free(HttpResponse *resp) {
    free(resp->data);
    resp->data = NULL;
    resp->len = 0;
    resp->cap = 0;
}
