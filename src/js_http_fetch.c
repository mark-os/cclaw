#define _POSIX_C_SOURCE 200809L
#include "js_http_fetch.h"
#include <curl/curl.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#define FETCH_MAX (512 * 1024)

/* V38: check if IP is private/loopback (SSRF protection) */
static int is_private_ip(const char *ip) {
    struct in_addr addr4;
    if (inet_pton(AF_INET, ip, &addr4) == 1) {
        uint32_t h = ntohl(addr4.s_addr);
        if ((h >> 24) == 127) return 1;
        if ((h >> 24) == 10) return 1;
        if ((h & 0xFFF00000) == 0xAC100000) return 1; /* 172.16.0.0/12 */
        if ((h >> 16) == ((192 << 8) | 168)) return 1;
        if ((h >> 16) == ((169 << 8) | 254)) return 1;
        if (h == 0) return 1;
        return 0;
    }
    struct in6_addr addr6;
    if (inet_pton(AF_INET6, ip, &addr6) == 1) {
        static const uint8_t lo[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
        if (memcmp(&addr6, lo, 16) == 0) return 1;
        static const uint8_t zero[16] = {0};
        if (memcmp(&addr6, zero, 16) == 0) return 1;
        if (addr6.s6_addr[0] == 0xfe && (addr6.s6_addr[1] & 0xc0) == 0x80) return 1;
        if ((addr6.s6_addr[0] & 0xfe) == 0xfc) return 1;
        return 0;
    }
    return 0;
}

/* Extract hostname from URL */
static int extract_host(const char *url, char *host, size_t host_cap) {
    const char *p = strstr(url, "://");
    if (!p) return -1;
    p += 3;
    const char *at = strchr(p, '@');
    const char *slash = strchr(p, '/');
    if (at && (!slash || at < slash)) p = at + 1;
    size_t i = 0;
    while (*p && *p != ':' && *p != '/' && *p != '?' && i < host_cap - 1) {
        host[i++] = *p++;
    }
    host[i] = '\0';
    return (i > 0) ? 0 : -1;
}

/* V38: check hostname against allowed_hosts list */
static int host_allowed(const char *host, char **allowed, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (strcasecmp(host, allowed[i]) == 0) return 1;
    }
    return 0;
}

/* V38: resolve hostname and check all IPs for SSRF */
static int host_resolves_to_private(const char *host) {
    struct addrinfo hints = {0}, *res, *rp;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0) return 1;
    for (rp = res; rp; rp = rp->ai_next) {
        char ip[INET6_ADDRSTRLEN];
        if (rp->ai_family == AF_INET) {
            struct sockaddr_in *s = (struct sockaddr_in *)rp->ai_addr;
            inet_ntop(AF_INET, &s->sin_addr, ip, sizeof(ip));
        } else if (rp->ai_family == AF_INET6) {
            struct sockaddr_in6 *s = (struct sockaddr_in6 *)rp->ai_addr;
            inet_ntop(AF_INET6, &s->sin6_addr, ip, sizeof(ip));
        } else continue;
        if (is_private_ip(ip)) { freeaddrinfo(res); return 1; }
    }
    freeaddrinfo(res);
    return 0;
}

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

    /* V38: no allowlist = no network */
    if (!allowed_hosts || hosts_count == 0) {
        r.error = strdup("no allowed_hosts configured");
        return r;
    }

    /* Validate scheme */
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        r.error = strdup("url must start with http:// or https://");
        return r;
    }

    /* Extract and validate host */
    char host[256];
    if (extract_host(url, host, sizeof(host)) != 0) {
        r.error = strdup("cannot parse host from url");
        return r;
    }

    if (!host_allowed(host, allowed_hosts, hosts_count)) {
        r.error = strdup("host not in allowed_hosts");
        return r;
    }

    if (host_resolves_to_private(host)) {
        r.error = strdup("host resolves to private IP");
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
