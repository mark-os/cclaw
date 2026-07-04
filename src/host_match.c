#define _POSIX_C_SOURCE 200809L
#include "host_match.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

int cidr_parse(const char *s, Cidr *out) {
    if (!s || !out) return -1;
    memset(out, 0, sizeof(*out));

    char buf[INET6_ADDRSTRLEN + 4];
    const char *slash = strchr(s, '/');
    size_t addr_len;
    int explicit_len = -1;

    if (slash) {
        addr_len = (size_t)(slash - s);
        if (addr_len >= sizeof(buf)) return -1;
        memcpy(buf, s, addr_len);
        buf[addr_len] = '\0';
        /* Prefix length must be a non-empty run of digits — reject "10.0.0.0/"
         * and "1.2.3.4/xyz" rather than letting atoi() default them to /0,
         * which would silently widen a malformed grant to allow-all. */
        const char *plen = slash + 1;
        if (*plen == '\0') return -1;
        for (const char *q = plen; *q; q++)
            if (*q < '0' || *q > '9') return -1;
        long plen_val = strtol(plen, NULL, 10);
        if (plen_val < 0 || plen_val > 128) return -1;
        explicit_len = (int)plen_val;
    } else {
        addr_len = strlen(s);
        if (addr_len >= sizeof(buf)) return -1;
        memcpy(buf, s, addr_len + 1);
    }

    struct in_addr a4;
    struct in6_addr a6;
    if (inet_pton(AF_INET, buf, &a4) == 1) {
        out->family = AF_INET;
        memcpy(out->prefix, &a4, 4);
        out->prefix_len = (explicit_len >= 0) ? explicit_len : 32;
        if (out->prefix_len < 0 || out->prefix_len > 32) return -1;
    } else if (inet_pton(AF_INET6, buf, &a6) == 1) {
        out->family = AF_INET6;
        memcpy(out->prefix, &a6, 16);
        out->prefix_len = (explicit_len >= 0) ? explicit_len : 128;
        if (out->prefix_len < 0 || out->prefix_len > 128) return -1;
    } else {
        return -1;
    }

    /* Zero out host bits in prefix for clean comparison */
    int max = (out->family == AF_INET) ? 4 : 16;
    for (int i = 0; i < max; i++) {
        int bit_start = i * 8;
        if (bit_start >= out->prefix_len) {
            out->prefix[i] = 0;
        } else if (bit_start + 8 > out->prefix_len) {
            int keep = out->prefix_len - bit_start;
            out->prefix[i] &= (unsigned char)(0xFF << (8 - keep));
        }
    }
    return 0;
}

/* Match host against rules. Case-insensitive — DNS names are case-insensitive. */
int host_match(char **rules, size_t n, const char *host) {
    if (!rules || !host) return 0;
    size_t hlen = strlen(host);
    for (size_t i = 0; i < n; i++) {
        const char *r = rules[i];
        if (!r) continue;
        if (r[0] == '.') {
            /* Suffix rule: ".github.com" matches "github.com" and
             * "api.github.com" but NOT "evilgithub.com" */
            size_t rlen = strlen(r);         /* includes leading dot */
            size_t base = rlen - 1;          /* "github.com" length */
            /* Exact match with the base (host == rule+1) */
            if (hlen == base && strcasecmp(host, r + 1) == 0) return 1;
            /* Suffix match: host ends with rule AND the match is at a dot boundary
             * (the rule itself starts with '.', so host ending with rule means
             * the char before the suffix in host IS the dot from rule) */
            if (hlen > rlen) {
                const char *tail = host + (hlen - rlen);
                if (strcasecmp(tail, r) == 0) return 1;
            }
        } else {
            /* Exact match */
            if (strcasecmp(host, r) == 0) return 1;
        }
    }
    return 0;
}

/* True if `token` is covered by `label` under registered-domain+subdomain
 * semantics: exact, or subdomain of it. A label already written as a suffix
 * rule (".x.y") keeps host_match's suffix behavior unchanged. */
static int label_covers(const char *label, const char *token) {
    char *one[1] = {(char *)label};
    if (host_match(one, 1, token)) return 1;
    if (label[0] != '.') {
        char suffixed[256];
        int n = snprintf(suffixed, sizeof(suffixed), ".%s", label);
        if (n > 0 && (size_t)n < sizeof(suffixed)) {
            char *sone[1] = {suffixed};
            if (host_match(sone, 1, token)) return 1;
        }
    }
    return 0;
}

int host_covered(char **labels, size_t n, const char *host) {
    if (!labels || !host) return 0;
    for (size_t i = 0; i < n; i++)
        if (labels[i] && label_covers(labels[i], host)) return 1;
    return 0;
}

static int is_host_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '-';
}

int host_in_text(char **labels, size_t n, const char *text,
                 char *out_host, size_t out_cap) {
    if (out_host && out_cap) out_host[0] = '\0';
    if (!labels || n == 0 || !text) return 0;
    const char *p = text;
    while (*p) {
        if (!is_host_char(*p)) { p++; continue; }
        const char *start = p;
        while (*p && is_host_char(*p)) p++;
        /* Trim edge dots/hyphens (JSON/prose punctuation, "-x" flags) */
        const char *s = start, *e = p;
        while (s < e && (*s == '.' || *s == '-')) s++;
        while (e > s && (e[-1] == '.' || e[-1] == '-')) e--;
        size_t len = (size_t)(e - s);
        if (len == 0 || len >= 254) continue;
        char token[254];
        memcpy(token, s, len);
        token[len] = '\0';
        for (size_t i = 0; i < n; i++) {
            if (!labels[i]) continue;
            if (label_covers(labels[i], token)) {
                if (out_host && out_cap) {
                    size_t c = len < out_cap - 1 ? len : out_cap - 1;
                    memcpy(out_host, token, c);
                    out_host[c] = '\0';
                }
                return 1;
            }
        }
    }
    return 0;
}

static int cidr_contains(const Cidr *rule, int family,
                         const unsigned char *addr) {
    if (rule->family != family) return 0;
    int bytes = rule->prefix_len / 8;
    int bits = rule->prefix_len % 8;
    if (bytes > 0 && memcmp(rule->prefix, addr, (size_t)bytes) != 0) return 0;
    if (bits > 0) {
        unsigned char mask = (unsigned char)(0xFF << (8 - bits));
        if ((rule->prefix[bytes] & mask) != (addr[bytes] & mask)) return 0;
    }
    return 1;
}

int cidr_match(const Cidr *rules, size_t n, int family,
               const unsigned char *addr) {
    if (!rules || !addr) return 0;
    for (size_t i = 0; i < n; i++)
        if (cidr_contains(&rules[i], family, addr)) return 1;
    return 0;
}

int granted_exact(const Cidr *rules, size_t n, int family,
                  const unsigned char *addr) {
    if (!rules || !addr) return 0;
    int max_len = (family == AF_INET) ? 32 : 128;
    int addr_bytes = (family == AF_INET) ? 4 : 16;
    for (size_t i = 0; i < n; i++) {
        if (rules[i].family != family) continue;
        if (rules[i].prefix_len != max_len) continue;
        if (memcmp(rules[i].prefix, addr, (size_t)addr_bytes) == 0) return 1;
    }
    return 0;
}
