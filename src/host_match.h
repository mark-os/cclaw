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

/* True if `host` is covered by any label in `labels[0..n)` under
 * registered-domain+subdomain semantics: exact match, subdomain of it, or
 * explicit ".suffix" label. Labels use the same semantics as host_in_text
 * (bare "x.y" implies both exact and ".x.y" suffix). Case-insensitive. */
int host_covered(char **labels, size_t n, const char *host);

/* Scan free-form text (tool-call args JSON, a shell command) for a host token
 * covered by any label. Tokens are maximal [A-Za-z0-9.-] runs, trimmed of
 * edge dots/hyphens. Labels use registered-domain+subdomain semantics: label
 * "chase.com" covers "chase.com" and "api.chase.com" but never
 * "mychase.com" or "chase.com.evil.co" (dot-boundary via host_match; a bare
 * label is treated as itself plus its ".label" suffix rule). Case-insensitive.
 * Returns 1 and copies the matched token into out_host (if non-NULL, always
 * NUL-terminated, truncated to out_cap) on first hit; 0 if clean. */
int host_in_text(char **labels, size_t n, const char *text,
                 char *out_host, size_t out_cap);

/* CIDR containment: true if (family, addr_bytes) falls within any rule. */
int cidr_match(const Cidr *rules, size_t n, int family,
               const unsigned char *addr);

/* Exact-address match: true only if a rule with prefix_len==max (32 or 128)
 * has addr equal to the candidate. Used for the metadata carve-out — a covering
 * CIDR must NOT authorize the metadata IP; only an exact grant works. */
int granted_exact(const Cidr *rules, size_t n, int family,
                  const unsigned char *addr);

#endif /* CCLAW_HOST_MATCH_H */
