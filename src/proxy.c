#define _GNU_SOURCE
#include "proxy.h"
#include "host_match.h"
#include "http_policy.h"
#include "log.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define PROXY_BACKLOG 8
#define RELAY_BUF_SIZE 4096
#define PREAMBLE_MAX 300

/* Structural metadata range — in CODE, not config (§6 guarantee).
 * 169.254.0.0/16 (v4 link-local, covers AWS/GCP/Azure metadata at
 * 169.254.169.254) and fe80::/10 (v6 link-local, covers cloud v6 metadata
 * endpoints). A granted CIDR that contains these ranges CANNOT authorize
 * access — only an exact literal grant works (the metadata carve-out).
 *
 * fd00:ec2::254 is AWS's IMDS endpoint over IPv6 — a ULA (fc00::/7), which
 * http_is_private_ip treats as ordinary private space, so without listing it
 * here a broad ULA grant (fd00::/8) would reach instance credentials. Listed
 * as an exact /128 so only a literal grant for it authorizes access.
 * IPv4-mapped IPv6 (::ffff:169.254.169.254) is NOT handled here — ip_to_bin
 * canonicalizes mapped addresses to their v4 form before any metadata test. */
static const Cidr METADATA_RANGES[] = {
    { .family = AF_INET,  .prefix = {169, 254, 0, 0}, .prefix_len = 16 },
    { .family = AF_INET6, .prefix = {0xfe, 0x80},     .prefix_len = 10 },
    { .family = AF_INET6,
      .prefix = {0xfd, 0x00, 0x0e, 0xc2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02, 0x54},
      .prefix_len = 128 },
};
#define METADATA_RANGE_COUNT 3

static int in_metadata_range(int family, const unsigned char *addr) {
    return cidr_match(METADATA_RANGES, METADATA_RANGE_COUNT, family, addr);
}

/* Shared core of the two numeric-address checks below: metadata carve-out
 * (exact grant only — a covering CIDR is never enough), then either
 * public-IP-allow or a CIDR-grant requirement. Kept as ONE function so the
 * metadata carve-out — the single most security-critical check in this file
 * — has exactly one implementation instead of two hand-kept-in-parity copies
 * that can silently drift (that drift is exactly how the host_decide/
 * addr_permitted asymmetry went unnoticed: see egress-filter.md Q7).
 *
 * allow_public distinguishes the two callers' trust contexts, not a
 * duplicated policy choice:
 *   - addr_permitted (allow_public=1): runs only AFTER a hostname already
 *     passed host_match — the grant was already spent, and this is purely a
 *     DNS-rebind recheck. Allowing the resolved public IP is exactly the
 *     grant paying off.
 *   - host_decide's numeric branch (allow_public=0): reached with NO
 *     hostname ever vetted (a raw IP literal skips host_match entirely), so
 *     a public IP needs its own CIDR grant (e.g. 0.0.0.0/0 + ::/0 for "any
 *     public IP") — otherwise any tool call phrasing its target as a literal
 *     IP bypasses the allowlist outright. */
static int numeric_ip_permitted(const ProxyContext *ctx, const char *ip,
                                int fam, const unsigned char *bin,
                                int allow_public) {
    if (in_metadata_range(fam, bin))
        return granted_exact(ctx->cidr_rules, ctx->cidr_rule_count, fam, bin);
    if (allow_public && !http_is_private_ip(ip)) return 1;
    return cidr_match(ctx->cidr_rules, ctx->cidr_rule_count, fam, bin);
}

/* Check if host is allowed via the partitioned rules: numeric IPs are judged
 * FIRST by numeric_ip_permitted() — before hostname rules are even
 * consulted, so a hostname wildcard ("*") can never leak into the metadata
 * carve-out. Non-numeric hosts fall through to host_match (exact + suffix +
 * "*"). Default-deny — empty/NULL rules deny everything. */
static int ip_to_bin(const char *ip, int *fam, unsigned char *buf16);
static int host_decide(const ProxyContext *ctx, const char *host) {
    /* Sensitive-host deny list: checked FIRST, before any allow logic.
     * Hostname-level deny is sufficient for numeric CONNECT too: a denied
     * hostname never passes RESOLVE, so its IP is never blessed, and
     * unblessed numeric CONNECT is already denied by the default-deny path. */
    if (ctx->deny_rules && host_covered(ctx->deny_rules, ctx->deny_count, host)) {
        LOG_WARN_("proxy deny host=%s reason=sensitive", host);
        return 0;
    }

    /* Numeric IP: CIDR pipeline only, no public-IP carve-out. Must run
     * before host_match — see numeric_ip_permitted's comment for why a
     * hostname wildcard must never reach here. */
    int fam;
    unsigned char bin[16];
    if (ip_to_bin(host, &fam, bin) == 0)
        return numeric_ip_permitted(ctx, host, fam, bin, /*allow_public=*/0);

    /* Hostname rules (exact/suffix/"*") */
    return host_match(ctx->host_rules, ctx->host_rule_count, host);
}

/* Parse an IP string to its binary form. Returns 0 + family/bytes on success. */
static int ip_to_bin(const char *ip, int *fam, unsigned char *buf16) {
    struct in_addr a4;
    if (inet_pton(AF_INET, ip, &a4) == 1) {
        *fam = AF_INET;
        memcpy(buf16, &a4, 4);
        return 0;
    }
    struct in6_addr a6;
    if (inet_pton(AF_INET6, ip, &a6) == 1) {
        /* Canonicalize IPv4-mapped IPv6 (::ffff:a.b.c.d) to its v4 form so the
         * metadata carve-out and CIDR checks cannot be bypassed by spelling a
         * v4 address (e.g. 169.254.169.254) in mapped notation. Every caller
         * routes through here, so the normalization is uniform. */
        static const unsigned char mapped_prefix[12] =
            {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (memcmp(a6.s6_addr, mapped_prefix, 12) == 0) {
            *fam = AF_INET;
            memcpy(buf16, &a6.s6_addr[12], 4);
            return 0;
        }
        *fam = AF_INET6;
        memcpy(buf16, &a6, 16);
        return 0;
    }
    return -1;
}

/* True if `ip` is in the blessed set and not expired. When found,
 * *via_hostname_out (may be NULL) reports whether the bless came from
 * hostname resolution. */
static int bless_contains(ProxyContext *ctx, const char *ip, int *via_hostname_out) {
    int fam;
    unsigned char bin[16];
    if (ip_to_bin(ip, &fam, bin) != 0) return 0;
    int len = (fam == AF_INET) ? 4 : 16;
    long now = (long)time(NULL);
    int found = 0;
    pthread_mutex_lock(&ctx->blessed_mu);
    for (int i = 0; i < ctx->blessed_count; i++) {
        ProxyBlessedAddr *b = &ctx->blessed[i];
        if (b->expiry <= now) continue;
        if (b->family == fam && memcmp(b->addr, bin, (size_t)len) == 0) {
            found = 1;
            if (via_hostname_out) *via_hostname_out = b->via_hostname;
            break;
        }
    }
    pthread_mutex_unlock(&ctx->blessed_mu);
    return found;
}

/* Add `ip` to the blessed set with a fresh TTL. Prunes expired entries and
 * evicts the oldest if the set is full. Refreshes the TTL on a duplicate
 * (via_hostname is sticky — OR-merged on refresh). */
static void bless_add(ProxyContext *ctx, const char *ip, int via_hostname) {
    int fam;
    unsigned char bin[16];
    if (ip_to_bin(ip, &fam, bin) != 0) return;
    int len = (fam == AF_INET) ? 4 : 16;
    long now = (long)time(NULL);
    pthread_mutex_lock(&ctx->blessed_mu);
    /* Drop expired entries (compact in place). */
    int w = 0;
    for (int i = 0; i < ctx->blessed_count; i++) {
        if (ctx->blessed[i].expiry > now) {
            if (w != i) ctx->blessed[w] = ctx->blessed[i];
            w++;
        }
    }
    ctx->blessed_count = w;
    /* Refresh if already present. */
    for (int i = 0; i < ctx->blessed_count; i++) {
        if (ctx->blessed[i].family == fam &&
            memcmp(ctx->blessed[i].addr, bin, (size_t)len) == 0) {
            ctx->blessed[i].expiry = now + PROXY_BLESS_TTL_SECS;
            ctx->blessed[i].via_hostname |= via_hostname;
            pthread_mutex_unlock(&ctx->blessed_mu);
            return;
        }
    }
    if (ctx->blessed_count >= PROXY_BLESS_MAX) {
        int oldest = 0;
        for (int i = 1; i < ctx->blessed_count; i++)
            if (ctx->blessed[i].expiry < ctx->blessed[oldest].expiry) oldest = i;
        ctx->blessed[oldest] = ctx->blessed[ctx->blessed_count - 1];
        ctx->blessed_count--;
    }
    ProxyBlessedAddr *b = &ctx->blessed[ctx->blessed_count++];
    b->family = fam;
    memset(b->addr, 0, sizeof(b->addr));
    memcpy(b->addr, bin, (size_t)len);
    b->expiry = now + PROXY_BLESS_TTL_SECS;
    b->via_hostname = via_hostname;
    pthread_mutex_unlock(&ctx->blessed_mu);
}

/* Remember an allowed target for the entry's network_hosts tag. Dedup'd,
 * capped at PROXY_CONTACTED_MAX (overflow drops silently — the tag is
 * provenance metadata, not policy). Called from conn threads. */
static void record_host(ProxyContext *ctx, const char *host) {
    if (!host || !host[0]) return;
    pthread_mutex_lock(&ctx->blessed_mu);
    for (int i = 0; i < ctx->contacted_count; i++) {
        if (strcmp(ctx->contacted[i], host) == 0) {
            pthread_mutex_unlock(&ctx->blessed_mu);
            return;
        }
    }
    if (ctx->contacted_count < PROXY_CONTACTED_MAX) {
        char *dup = strdup(host);
        if (dup) ctx->contacted[ctx->contacted_count++] = dup;
    }
    pthread_mutex_unlock(&ctx->blessed_mu);
}

/* Remember a denied target for the tool result summary. Same dedup/cap pattern
 * as record_host — the list is best-effort metadata, not policy. */
static void record_denied(ProxyContext *ctx, const char *host) {
    if (!host || !host[0]) return;
    pthread_mutex_lock(&ctx->blessed_mu);
    for (int i = 0; i < ctx->denied_count; i++) {
        if (strcmp(ctx->denied[i], host) == 0) {
            pthread_mutex_unlock(&ctx->blessed_mu);
            return;
        }
    }
    if (ctx->denied_count < PROXY_CONTACTED_MAX) {
        char *dup = strdup(host);
        if (dup) ctx->denied[ctx->denied_count++] = dup;
    }
    pthread_mutex_unlock(&ctx->blessed_mu);
}

/* A resolved address is permitted per the egress-filter.md §4 pipeline:
 *   1. Metadata range → exact grant only (CIDR grants cannot reach metadata)
 *   2. Public IP → ALLOW (the hostname that resolved here already spent its
 *      grant via host_match — see numeric_ip_permitted's comment)
 *   3. Private IP in granted CIDR → ALLOW
 *   4. Otherwise → DENY
 * Ordering is security-critical: metadata check MUST precede CIDR check. */
static int addr_permitted(const ProxyContext *ctx, const char *ip) {
    int fam;
    unsigned char bin[16];
    if (ip_to_bin(ip, &fam, bin) != 0) return 0;
    return numeric_ip_permitted(ctx, ip, fam, bin, /*allow_public=*/1);
}

/* Resolve host (allowlist already checked by the caller), drop disallowed
 * addresses, bless the survivors with a TTL. On success writes the first
 * blessed IP into ip_out and returns 0; returns -1 if nothing is permitted. */
static int resolve_and_bless(ProxyContext *ctx, const char *host,
                             char *ip_out, size_t ip_cap) {
    struct addrinfo hints = {0}, *res, *rp;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0)
        return -1;
    int got = 0;
    for (rp = res; rp; rp = rp->ai_next) {
        char ip[INET6_ADDRSTRLEN];
        if (rp->ai_family == AF_INET) {
            struct sockaddr_in *s = (struct sockaddr_in *)rp->ai_addr;
            if (!inet_ntop(AF_INET, &s->sin_addr, ip, sizeof(ip))) continue;
        } else if (rp->ai_family == AF_INET6) {
            struct sockaddr_in6 *s = (struct sockaddr_in6 *)rp->ai_addr;
            if (!inet_ntop(AF_INET6, &s->sin6_addr, ip, sizeof(ip))) continue;
        } else continue;
        if (!addr_permitted(ctx, ip)) continue;
        bless_add(ctx, ip, /*via_hostname=*/1);
        if (!got) {
            snprintf(ip_out, ip_cap, "%s", ip);
            got = 1;
        }
    }
    freeaddrinfo(res);
    return got ? 0 : -1;
}

/* Dial a literal IP:port directly — no name resolution, so the dialed address
 * is exactly the blessed one (no resolve/connect TOCTOU). Returns fd or -1. */
static int dial_ip(const char *ip, uint16_t port) {
    struct in_addr a4;
    if (inet_pton(AF_INET, ip, &a4) == 1) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        struct sockaddr_in sa = {0};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        sa.sin_addr = a4;
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { close(fd); return -1; }
        return fd;
    }
    struct in6_addr a6;
    if (inet_pton(AF_INET6, ip, &a6) == 1) {
        int fd = socket(AF_INET6, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        struct sockaddr_in6 sa = {0};
        sa.sin6_family = AF_INET6;
        sa.sin6_port = htons(port);
        sa.sin6_addr = a6;
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { close(fd); return -1; }
        return fd;
    }
    return -1;
}

/* Reserve a relay slot. Returns 1 on success, 0 if at the per-call cap. */
static int relay_acquire(ProxyContext *ctx) {
    int ok = 0;
    pthread_mutex_lock(&ctx->blessed_mu);
    if (ctx->relay_count < PROXY_MAX_RELAYS) { ctx->relay_count++; ok = 1; }
    pthread_mutex_unlock(&ctx->blessed_mu);
    return ok;
}

static void relay_release(ProxyContext *ctx) {
    pthread_mutex_lock(&ctx->blessed_mu);
    if (ctx->relay_count > 0) ctx->relay_count--;
    pthread_mutex_unlock(&ctx->blessed_mu);
}

/* Relay data bidirectionally between two fds until one closes.
 * net_shim.c's splice_both() is a near-identical loop — deliberately NOT
 * shared. net_shim is a separate translation unit specifically so it links
 * no policy/resolver/secret/DB symbols (see its file header); merging this
 * into a shared helper module risks that isolation growing over time. Leave
 * the duplication. */
static void relay(int fd1, int fd2) {
    struct pollfd fds[2];
    fds[0].fd = fd1;
    fds[0].events = POLLIN;
    fds[1].fd = fd2;
    fds[1].events = POLLIN;

    char buf[RELAY_BUF_SIZE];
    while (1) {
        int ret = poll(fds, 2, 30000);
        if (ret <= 0) break;

        for (int i = 0; i < 2; i++) {
            if (fds[i].revents & (POLLIN | POLLHUP)) {
                ssize_t n = read(fds[i].fd, buf, sizeof(buf));
                if (n <= 0) return;
                int dst = (i == 0) ? fd2 : fd1;
                ssize_t written = 0;
                while (written < n) {
                    ssize_t w = write(dst, buf + written, (size_t)(n - written));
                    if (w <= 0) return;
                    written += w;
                }
            }
            if (fds[i].revents & (POLLERR | POLLNVAL))
                return;
        }
    }
}

/* Read a line from fd (up to max-1 bytes). Returns length or -1. */
static int proxy_read_line(int fd, char *buf, int max) {
    int pos = 0;
    while (pos < max - 1) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n <= 0) return -1;
        if (c == '\n') break;
        buf[pos++] = c;
    }
    buf[pos] = '\0';
    return pos;
}

/* sni_check()'s bounded ClientHello walk — result codes. */
#define SNI_PARSE_INCOMPLETE (-1) /* need more peeked bytes */
#define SNI_PARSE_FAIL_OPEN  0    /* not TLS / not ClientHello / malformed */
#define SNI_PARSE_FOUND      1    /* sni_out filled */
#define SNI_PARSE_NO_SNI     2    /* well-formed ClientHello, no SNI ext (D7) */

/* Hand-rolled, bounded walk of a single-record TLS ClientHello looking for
 * the server_name (SNI) extension. `have` bytes have been peeked so far (may
 * grow across calls as more of the record arrives). Every length read is
 * checked against both `have` (what's actually been peeked) and `end` (the
 * handshake body's own declared length) before being dereferenced or used to
 * skip ahead — a malformed/adversarial length can't walk past either bound.
 * Multi-record ClientHellos (split across TLS records) are not supported —
 * treated as SNI_PARSE_FAIL_OPEN once inconsistent with the single-record
 * assumption, which only narrows an already-passing hostname grant. */
static int parse_client_hello_sni(const unsigned char *buf, size_t have,
                                  char *sni_out, size_t sni_cap) {
    if (have < 5) return SNI_PARSE_INCOMPLETE;
    if (buf[0] != 0x16) return SNI_PARSE_FAIL_OPEN;        /* not a handshake record */
    if (buf[1] != 0x03) return SNI_PARSE_FAIL_OPEN;        /* not TLS 1.x */

    if (have < 9) return SNI_PARSE_INCOMPLETE;
    if (buf[5] != 0x01) return SNI_PARSE_FAIL_OPEN;        /* not a ClientHello */
    size_t hs_len = ((size_t)buf[6] << 16) | ((size_t)buf[7] << 8) | buf[8];
    size_t end = 9 + hs_len;                                /* end of handshake body */

    size_t pos = 9;
#define SNI_NEED(n) \
    do { \
        if (pos + (n) > end) return SNI_PARSE_FAIL_OPEN; \
        if (pos + (n) > have) return SNI_PARSE_INCOMPLETE; \
    } while (0)

    SNI_NEED(2); pos += 2;                                  /* client_version */
    SNI_NEED(32); pos += 32;                                /* random */

    SNI_NEED(1);
    size_t session_id_len = buf[pos]; pos += 1;
    SNI_NEED(session_id_len); pos += session_id_len;

    SNI_NEED(2);
    size_t cipher_len = ((size_t)buf[pos] << 8) | buf[pos + 1]; pos += 2;
    SNI_NEED(cipher_len); pos += cipher_len;

    SNI_NEED(1);
    size_t comp_len = buf[pos]; pos += 1;
    SNI_NEED(comp_len); pos += comp_len;

    if (pos >= end) return SNI_PARSE_NO_SNI;                /* no extensions block */

    SNI_NEED(2);
    size_t ext_total = ((size_t)buf[pos] << 8) | buf[pos + 1]; pos += 2;
    size_t ext_end = pos + ext_total;
    if (ext_end > end) return SNI_PARSE_FAIL_OPEN;

    while (pos < ext_end) {
        SNI_NEED(4);
        unsigned ext_type = ((unsigned)buf[pos] << 8) | buf[pos + 1];
        size_t ext_len = ((size_t)buf[pos + 2] << 8) | buf[pos + 3];
        pos += 4;
        SNI_NEED(ext_len);

        if (ext_type == 0x0000) {                           /* server_name */
            size_t sp = pos;
            size_t sp_end = sp + ext_len;
            if (sp + 2 > sp_end) return SNI_PARSE_FAIL_OPEN;
            size_t list_len = ((size_t)buf[sp] << 8) | buf[sp + 1];
            sp += 2;
            if (sp + list_len > sp_end) return SNI_PARSE_FAIL_OPEN;
            if (sp + 3 > sp_end) return SNI_PARSE_FAIL_OPEN;
            unsigned name_type = buf[sp];
            size_t name_len = ((size_t)buf[sp + 1] << 8) | buf[sp + 2];
            sp += 3;
            if (sp + name_len > sp_end) return SNI_PARSE_FAIL_OPEN;
            if (name_type != 0 || name_len == 0 || name_len >= sni_cap)
                return SNI_PARSE_FAIL_OPEN;
            memcpy(sni_out, buf + sp, name_len);
            sni_out[name_len] = '\0';
            return SNI_PARSE_FOUND;
        }
        pos += ext_len;
    }
#undef SNI_NEED
    return SNI_PARSE_NO_SNI;                                /* no SNI extension present */
}

#define SNI_PEEK_MAX 2048
#define SNI_PEEK_BUDGET_MS 2000

/* Peek (never consume — relay() still needs to see these bytes) at the start
 * of the client's byte stream, looking for a TLS ClientHello's SNI hostname.
 * Called for connections whose authority derives from a hostname rule — a
 * hostname CONNECT, or a numeric CONNECT to a hostname-blessed IP (Q6 gate):
 * once an allowlisted hostname blesses an IP, a CONNECT to that same IP is
 * otherwise admitted regardless of what vhost it actually targets — on
 * shared-IP CDN hosting this reaches unauthorized tenants sharing the IP.
 * Fail-open (return 1) when the bytes aren't parseable as a single-record
 * ClientHello within the peek budget — a hostname grant doesn't imply the
 * protocol is HTTPS. One exception (D7): a *well-formed* ClientHello that
 * omits the SNI extension is denied on sni_strict_port (443) — every modern
 * HTTPS client sends SNI, so omitting it there is precisely the shape of a
 * deliberate gate evasion, while ECH still presents an outer SNI and non-TLS
 * protocols on 443 don't parse as a hello (still fail-open). RFC 6066
 * IP-literal clients that legitimately skip SNI are covered by the exact-IP
 * grant exemption upstream. When SNI *is* parsed, checked against the full
 * granted host-rule set (not just the specific rule that authorized this
 * connection) — mirrors how bless_contains() already works at the IP level.
 * On deny (return 0) *deny_reason names the arm for the log line. */
static int sni_check(const ProxyContext *ctx, int client_fd, uint16_t port,
                     const char **deny_reason) {
    unsigned char buf[SNI_PEEK_MAX];
    size_t have = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long deadline_ms = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L + SNI_PEEK_BUDGET_MS;

    char sni[256];
    for (;;) {
        ssize_t n = recv(client_fd, buf, sizeof(buf), MSG_PEEK);
        if (n > 0) have = (size_t)n;

        int rc = parse_client_hello_sni(buf, have, sni, sizeof(sni));
        if (rc == SNI_PARSE_FOUND) {
            if (host_match(ctx->host_rules, ctx->host_rule_count, sni))
                return 1;
            *deny_reason = "sni_mismatch";
            return 0;
        }
        if (rc == SNI_PARSE_NO_SNI) {
            if (port != ctx->sni_strict_port)
                return 1;
            *deny_reason = "sni_absent";
            return 0;
        }
        if (rc == SNI_PARSE_FAIL_OPEN)
            return 1;

        /* SNI_PARSE_INCOMPLETE: wait for more bytes, bounded by the budget. */
        if (have >= sizeof(buf)) return 1;   /* full budget peeked, still incomplete */
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long now_ms = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
        long remaining = deadline_ms - now_ms;
        if (remaining <= 0) return 1;

        /* poll() returns immediately if the already-peeked bytes are still
         * unread (level-triggered), so this doesn't block waiting for new
         * data specifically — it's a scheduling slice. The top-of-loop
         * deadline check is what actually bounds total wall-clock spent
         * here when a truncated ClientHello never grows. */
        struct pollfd pfd = { .fd = client_fd, .events = POLLIN };
        int slice = remaining < 20 ? (int)remaining : 20;
        poll(&pfd, 1, slice);
    }
}

/* Handle a single client connection from a shell child. One unified policy
 * path: a hostname is allowlist-checked, resolved, SSRF-filtered, and blessed;
 * a numeric target is dialed only if it was blessed by a prior RESOLVE or is an
 * explicitly granted literal IP. The dialed address is always one we blessed. */
static void handle_client(ProxyContext *ctx, int client_fd) {
    char preamble[PREAMBLE_MAX];
    int len = proxy_read_line(client_fd, preamble, PREAMBLE_MAX);
    if (len <= 0) { close(client_fd); return; }

    /* RESOLVE request: "RESOLVE <host>" */
    if (strncmp(preamble, "RESOLVE ", 8) == 0) {
        const char *host = preamble + 8;
        if (!host_decide(ctx, host)) {
            LOG_WARN_("proxy deny host=%s reason=policy", host);
            record_denied(ctx, host);
            write(client_fd, "ERROR denied\n", 13);
            close(client_fd);
            return;
        }
        char ip[INET6_ADDRSTRLEN];
        if (resolve_and_bless(ctx, host, ip, sizeof(ip)) == 0) {
            record_host(ctx, host);
            char resp[INET6_ADDRSTRLEN + 8];
            int n = snprintf(resp, sizeof(resp), "ADDR %s\n", ip);
            write(client_fd, resp, (size_t)n);
        } else {
            LOG_WARN_("proxy deny host=%s reason=resolve_failed", host);
            record_denied(ctx, host);
            write(client_fd, "ERROR resolve\n", 14);
        }
        close(client_fd);
        return;
    }

    /* Connect request: "host:port" or "ip:port" */
    char *colon = strrchr(preamble, ':');
    if (!colon) {
        write(client_fd, "ERROR bad preamble\n", 19);
        close(client_fd);
        return;
    }
    *colon = '\0';
    const char *target = preamble;
    uint16_t port = (uint16_t)atoi(colon + 1);

    char resolved[INET6_ADDRSTRLEN];
    const char *dial;
    int fam;
    unsigned char bin[16];
    int sni_gate = 0;
    if (ip_to_bin(target, &fam, bin) == 0) {
        /* Numeric target: must be blessed by a prior RESOLVE, or an explicitly
         * granted literal IP. A literal IP that is neither (e.g. a raw
         * 1.2.3.4 SSRF attempt) is refused — there is no resolve to bless it. */
        int via_host_bless = 0;
        if (bless_contains(ctx, target, &via_host_bless)) {
            /* blessed by a prior RESOLVE — the hostname was recorded there.
             * A hostname-derived bless carries the Q6 SNI gate along to the
             * numeric CONNECT (the common path: getaddrinfo shim RESOLVEs,
             * then the client dials the returned IP) — otherwise the gate on
             * the hostname branch below is a fence with no side walls. An
             * exact literal-IP grant is exempt: the operator vouched for the
             * IP itself (same exact-vs-covering distinction as the metadata
             * carve-out); a covering CIDR authorizes reaching a range, not
             * any particular vhost identity behind one address. */
            dial = target;
            if (via_host_bless &&
                !granted_exact(ctx->cidr_rules, ctx->cidr_rule_count, fam, bin))
                sni_gate = 1;
        } else if (host_decide(ctx, target)) {
            bless_add(ctx, target, /*via_hostname=*/0);
            record_host(ctx, target);
            dial = target;
        } else {
            LOG_WARN_("proxy deny host=%s reason=policy", target);
            record_denied(ctx, target);
            write(client_fd, "DENIED\n", 7);
            close(client_fd);
            return;
        }
    } else {
        /* Hostname target: allowlist, then resolve + SSRF-filter + bless.
         * ip_to_bin() failing above means only host_match (via host_decide)
         * can have authorized this branch — CIDR/exact-grant checks are
         * unreachable for non-numeric input. */
        if (!host_decide(ctx, target)) {
            LOG_WARN_("proxy deny host=%s reason=policy", target);
            record_denied(ctx, target);
            write(client_fd, "DENIED\n", 7);
            close(client_fd);
            return;
        }
        sni_gate = 1;
        if (resolve_and_bless(ctx, target, resolved, sizeof(resolved)) != 0) {
            LOG_WARN_("proxy deny host=%s reason=resolve_failed", target);
            record_denied(ctx, target);
            write(client_fd, "ERROR resolve\n", 14);
            close(client_fd);
            return;
        }
        record_host(ctx, target);
        dial = resolved;
    }

    /* Bound simultaneous relays per call — refuse cleanly when at the cap. */
    if (!relay_acquire(ctx)) {
        LOG_INFO_("proxy relay_full host=%s", target);
        write(client_fd, "DENIED\n", 7);
        close(client_fd);
        return;
    }

    int remote_fd = dial_ip(dial, port);
    if (remote_fd < 0) {
        write(client_fd, "ERROR connect\n", 14);
        close(client_fd);
        relay_release(ctx);
        return;
    }

    write(client_fd, "OK\n", 3);

    /* Shared-IP CDN check (Q6): a hostname-authorized connection — whether
     * CONNECTed by name or by a hostname-blessed IP — is only as trustworthy
     * as the SNI it actually presents once the IP is shared across unrelated
     * vhosts. Fail-open when unparseable — a hostname grant doesn't imply
     * HTTPS — except a well-formed no-SNI hello on :443, denied per D7 (see
     * sni_check). */
    const char *sni_reason = NULL;
    if (sni_gate && !sni_check(ctx, client_fd, port, &sni_reason)) {
        LOG_WARN_("proxy deny host=%s reason=%s", target, sni_reason);
        close(remote_fd);
        close(client_fd);
        relay_release(ctx);
        return;
    }

    relay(client_fd, remote_fd);
    close(remote_fd);
    close(client_fd);
    relay_release(ctx);
}

/* Per-connection thread */
struct conn_args {
    ProxyContext *ctx;
    int fd;
};

static void *conn_thread(void *arg) {
    struct conn_args *ca = (struct conn_args *)arg;
    ProxyContext *ctx = ca->ctx;
    handle_client(ctx, ca->fd);
    free(ca);
    pthread_mutex_lock(&ctx->blessed_mu);
    ctx->conn_active--;
    if (ctx->conn_active == 0)
        pthread_cond_broadcast(&ctx->conn_cond);
    pthread_mutex_unlock(&ctx->blessed_mu);
    return NULL;
}

/* Main proxy accept loop (runs in dedicated thread). */
static void *proxy_loop(void *arg) {
    ProxyContext *ctx = (ProxyContext *)arg;

    while (ctx->running) {
        struct pollfd pfd = { .fd = ctx->listen_fd, .events = POLLIN };
        int ret = poll(&pfd, 1, 500);
        if (ret <= 0) continue;

        int client = accept(ctx->listen_fd, NULL, NULL);
        if (client < 0) continue;

        /* Cap total in-flight conn_threads (resolving + relaying) — bounds a
         * hostname whose DNS never responds from piling up unbounded blocked
         * threads. There is no thread yet to write a DENIED response, so this
         * is a silent TCP-level drop, not a proxy-protocol refusal. */
        pthread_mutex_lock(&ctx->blessed_mu);
        if (ctx->conn_active >= PROXY_MAX_PENDING) {
            pthread_mutex_unlock(&ctx->blessed_mu);
            close(client);
            continue;
        }
        ctx->conn_active++;
        pthread_mutex_unlock(&ctx->blessed_mu);

        /* Spawn per-connection thread (detached) */
        struct conn_args *ca = malloc(sizeof(*ca));
        if (!ca) {
            close(client);
            pthread_mutex_lock(&ctx->blessed_mu);
            ctx->conn_active--;
            if (ctx->conn_active == 0)
                pthread_cond_broadcast(&ctx->conn_cond);
            pthread_mutex_unlock(&ctx->blessed_mu);
            continue;
        }
        ca->ctx = ctx;
        ca->fd = client;

        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &attr, conn_thread, ca) != 0) {
            pthread_mutex_lock(&ctx->blessed_mu);
            ctx->conn_active--;
            if (ctx->conn_active == 0)
                pthread_cond_broadcast(&ctx->conn_cond);
            pthread_mutex_unlock(&ctx->blessed_mu);
            close(client);
            free(ca);
        }
        pthread_attr_destroy(&attr);
    }

    return NULL;
}

/* Bind + listen on the per-call UDS in `dir` (the agent folder), but do not
 * start serving yet. Splitting bind from serve lets the broker create the
 * socket while still single-threaded, fork the sandbox (a single-threaded fork
 * — no fork-in-threaded-process hazard), and only then start the accept thread.
 * Returns 0 on success. */
int proxy_bind(ProxyContext *ctx, const char *dir,
               char **hosts, size_t host_count,
               char **deny_rules, size_t deny_count) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->listen_fd = -1;
    pthread_mutex_init(&ctx->blessed_mu, NULL);
    pthread_cond_init(&ctx->conn_cond, NULL);
    ctx->hosts = hosts;          /* borrowed — must outlive the proxy */
    ctx->host_count = host_count;
    ctx->deny_rules = deny_rules;  /* borrowed — hostname labels only */
    ctx->deny_count = deny_count;
    ctx->sni_strict_port = 443;

    /* Partition grants into hostname rules vs CIDR/IP rules.
     * A grant containing '/' is a CIDR. A bare IP (parseable by inet_pton)
     * becomes a full-length CIDR (/32 or /128). Everything else is a hostname
     * rule (exact or suffix). */
    ctx->host_rules = NULL;
    ctx->host_rule_count = 0;
    ctx->cidr_rules = NULL;
    ctx->cidr_rule_count = 0;

    int fd = -1;  /* function-scope so the `fail` cleanup can close it */

    if (host_count > 0) {
        ctx->host_rules = malloc(host_count * sizeof(char *));
        ctx->cidr_rules = malloc(host_count * sizeof(Cidr));
        if (!ctx->host_rules || !ctx->cidr_rules) goto fail;
        for (size_t i = 0; i < host_count; i++) {
            if (!hosts[i]) continue;
            Cidr c;
            if (cidr_parse(hosts[i], &c) == 0) {
                ctx->cidr_rules[ctx->cidr_rule_count++] = c;
            } else {
                /* hostname rule — borrowed pointer, same lifetime as hosts[] */
                ctx->host_rules[ctx->host_rule_count++] = hosts[i];
            }
        }
    }

    /* Per-call socket path in `dir` (the agent folder, not the agent-visible
     * workspace): unique per process so concurrent brokers (each its own pid)
     * never collide. Sequential reuse within one process is covered by the
     * unlink below and proxy_stop's unlink. */
    size_t plen = strlen(dir) + 32;
    ctx->sock_path = malloc(plen);
    if (!ctx->sock_path) goto fail;
    snprintf(ctx->sock_path, plen, "%s/.proxy.%d.sock", dir, (int)getpid());

    /* Remove stale socket */
    unlink(ctx->sock_path);

    /* Create UDS listener (CLOEXEC so it never rides into the sandbox exec) */
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) goto fail;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(ctx->sock_path) >= sizeof(addr.sun_path)) goto fail;
    strncpy(addr.sun_path, ctx->sock_path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) goto fail;

    if (listen(fd, PROXY_BACKLOG) != 0) goto fail;

    ctx->listen_fd = fd;
    return 0;

fail:
    /* Every error path lands here: free the just-allocated partition arrays
     * (proxy_stop is NOT called on a failed bind), the socket path, and the fd. */
    if (fd >= 0) close(fd);
    if (ctx->sock_path) {
        unlink(ctx->sock_path);
        free(ctx->sock_path);
        ctx->sock_path = NULL;
    }
    free(ctx->host_rules);  ctx->host_rules = NULL;  ctx->host_rule_count = 0;
    free(ctx->cidr_rules);  ctx->cidr_rules = NULL;  ctx->cidr_rule_count = 0;
    return -1;
}

/* Start the accept-loop thread (joined in proxy_stop). Call after proxy_bind,
 * and in the broker after forking the sandbox. Returns 0 on success. */
int proxy_serve(ProxyContext *ctx) {
    if (ctx->listen_fd < 0) return -1;
    ctx->running = 1;
    if (pthread_create(&ctx->thread, NULL, proxy_loop, ctx) != 0) {
        ctx->running = 0;
        return -1;
    }
    ctx->thread_started = 1;
    return 0;
}

void proxy_stop(ProxyContext *ctx) {
    ctx->running = 0;
    if (ctx->thread_started) {
        pthread_join(ctx->thread, NULL);
        ctx->thread_started = 0;
    }
    /* Wait for all in-flight conn_threads to finish */
    pthread_mutex_lock(&ctx->blessed_mu);
    while (ctx->conn_active > 0)
        pthread_cond_wait(&ctx->conn_cond, &ctx->blessed_mu);
    pthread_mutex_unlock(&ctx->blessed_mu);
    if (ctx->listen_fd >= 0) {
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
    }
    if (ctx->sock_path) {
        unlink(ctx->sock_path);
        free(ctx->sock_path);
        ctx->sock_path = NULL;
    }
    for (int i = 0; i < ctx->contacted_count; i++) free(ctx->contacted[i]);
    ctx->contacted_count = 0;
    for (int i = 0; i < ctx->denied_count; i++) free(ctx->denied[i]);
    ctx->denied_count = 0;
    ctx->hosts = NULL;          /* borrowed — not owned, do not free */
    ctx->host_count = 0;
    ctx->deny_rules = NULL;     /* borrowed — not owned, do not free */
    ctx->deny_count = 0;
    free(ctx->host_rules);      /* owned partition array (pointers are borrowed) */
    ctx->host_rules = NULL;
    ctx->host_rule_count = 0;
    free(ctx->cidr_rules);      /* owned parsed CIDR array */
    ctx->cidr_rules = NULL;
    ctx->cidr_rule_count = 0;
    pthread_mutex_destroy(&ctx->blessed_mu);
    pthread_cond_destroy(&ctx->conn_cond);
}

const char *proxy_sock_path(const ProxyContext *ctx) {
    return ctx->sock_path;
}

/* Append one JSON-string-escaped byte. Hostnames are plain, but the preamble
 * is written by the sandboxed child — escape defensively so the output is
 * always valid JSON. */
static void hosts_json_escape_ch(char *out, size_t *o, unsigned char c) {
    if (c == '"' || c == '\\') { out[(*o)++] = '\\'; out[(*o)++] = (char)c; }
    else if (c < 0x20) *o += (size_t)sprintf(out + *o, "\\u%04x", c);
    else out[(*o)++] = (char)c;
}

char *proxy_hosts_json(ProxyContext *ctx) {
    pthread_mutex_lock(&ctx->blessed_mu);
    if (ctx->contacted_count == 0) {
        pthread_mutex_unlock(&ctx->blessed_mu);
        return NULL;
    }
    size_t cap = 3;  /* "[" + "]" + NUL */
    for (int i = 0; i < ctx->contacted_count; i++)
        cap += strlen(ctx->contacted[i]) * 6 + 3;  /* worst-case \uXXXX + quotes + comma */
    char *out = malloc(cap);
    if (!out) {
        pthread_mutex_unlock(&ctx->blessed_mu);
        return NULL;
    }
    size_t o = 0;
    out[o++] = '[';
    for (int i = 0; i < ctx->contacted_count; i++) {
        if (i) out[o++] = ',';
        out[o++] = '"';
        for (const char *p = ctx->contacted[i]; *p; p++)
            hosts_json_escape_ch(out, &o, (unsigned char)*p);
        out[o++] = '"';
    }
    out[o++] = ']';
    out[o] = '\0';
    pthread_mutex_unlock(&ctx->blessed_mu);
    return out;
}

char *proxy_denied_summary(ProxyContext *ctx) {
    pthread_mutex_lock(&ctx->blessed_mu);
    if (ctx->denied_count == 0) {
        pthread_mutex_unlock(&ctx->blessed_mu);
        return NULL;
    }
    /* Build: "\ncclaw: proxy blocked host(s) not in allowlist: foo.com, bar.com\n" */
    size_t cap = 128;
    for (int i = 0; i < ctx->denied_count; i++)
        cap += strlen(ctx->denied[i]) + 4;
    char *out = malloc(cap);
    if (!out) { pthread_mutex_unlock(&ctx->blessed_mu); return NULL; }
    size_t o = 0;
    o += (size_t)snprintf(out + o, cap - o,
        "\ncclaw: proxy blocked host(s) not in allowlist: ");
    for (int i = 0; i < ctx->denied_count; i++) {
        if (i > 0) o += (size_t)snprintf(out + o, cap - o, ", ");
        o += (size_t)snprintf(out + o, cap - o, "%s", ctx->denied[i]);
    }
    o += (size_t)snprintf(out + o, cap - o, "\n");
    pthread_mutex_unlock(&ctx->blessed_mu);
    return out;
}
