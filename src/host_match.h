#ifndef CCLAW_HOST_MATCH_H
#define CCLAW_HOST_MATCH_H

#include <stddef.h>

/* Parsed CIDR rule: covers both explicit CIDRs ("10.0.0.0/8") and literal IPs
 * (stored as /32 or /128). Binary representation enables spelling-independent
 * matching and fast masked compare. */
typedef struct {
    int family;                 /* AF_INET or AF_INET6 */
    unsigned char prefix[16];   /* network-byte-order prefix bits */
    int prefix_len;             /* 0-32 (v4) or 0-128 (v6) */
} Cidr;

/* Parse a CIDR string ("a.b.c.d/len", "ipv6/len", or bare IP → /32 or /128).
 * Returns 0 on success, -1 on parse failure. */
int cidr_parse(const char *s, Cidr *out);

/* Host-string matcher: exact or suffix (rule starting with '.').
 * Suffix ".github.com" matches "github.com" and "api.github.com"
 * but NOT "evilgithub.com" (dot-boundary enforced).
 * TODO Q3: wildcard-label (*.x) — not implemented this cut. */
int host_match(char **rules, size_t n, const char *host);

/* CIDR containment: true if (family, addr_bytes) falls within any rule. */
int cidr_match(const Cidr *rules, size_t n, int family,
               const unsigned char *addr);

/* Exact-address match: true only if a rule with prefix_len==max (32 or 128)
 * has addr equal to the candidate. Used for the metadata carve-out — a covering
 * CIDR must NOT authorize the metadata IP; only an exact grant works. */
int granted_exact(const Cidr *rules, size_t n, int family,
                  const unsigned char *addr);

#endif /* CCLAW_HOST_MATCH_H */
