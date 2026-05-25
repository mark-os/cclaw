#define _POSIX_C_SOURCE 200809L
#include "http_policy.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

int http_is_private_ip(const char *ip) {
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

int http_extract_host(const char *url, char *host, size_t host_cap) {
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

static int host_in_list(const char *host, char **list, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (strcasecmp(host, list[i]) == 0) return 1;
    }
    return 0;
}

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
        if (http_is_private_ip(ip)) { freeaddrinfo(res); return 1; }
    }
    freeaddrinfo(res);
    return 0;
}

int http_check_policy(const char *url, const HttpPolicy *policy,
                      char *err_buf, size_t err_cap) {
    if (!policy) return 0; /* NULL policy = unrestricted */

    /* Validate scheme */
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        if (err_buf) snprintf(err_buf, err_cap, "url must start with http:// or https://");
        return -1;
    }

    char host[256];
    if (http_extract_host(url, host, sizeof(host)) != 0) {
        if (err_buf) snprintf(err_buf, err_cap, "cannot parse host from url");
        return -1;
    }

    /* Check blocked list first */
    if (policy->blocked_hosts && policy->blocked_count > 0) {
        if (host_in_list(host, policy->blocked_hosts, policy->blocked_count)) {
            if (err_buf) snprintf(err_buf, err_cap, "host is blocked");
            return -1;
        }
    }

    /* Check allowed list (empty allowed = allow all) */
    if (policy->allowed_hosts && policy->allowed_count > 0) {
        if (!host_in_list(host, policy->allowed_hosts, policy->allowed_count)) {
            if (err_buf) snprintf(err_buf, err_cap, "host not in allowed_hosts");
            return -1;
        }
    }

    /* SSRF: block private IPs */
    if (policy->block_private) {
        if (host_resolves_to_private(host)) {
            if (err_buf) snprintf(err_buf, err_cap, "host resolves to private IP");
            return -1;
        }
    }

    return 0;
}
