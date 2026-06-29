#define _GNU_SOURCE
#include "proxy.h"
#include "host_match.h"
#include "http_policy.h"
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

/* Check if host is allowed via the partitioned rules: hostname rules go through
 * host_match (exact + suffix), numeric IPs go through cidr_match (containment).
 * Default-deny — empty/NULL rules deny everything.
 * For numeric IPs in the metadata range, the carve-out applies: only an exact
 * literal grant (/32 or /128) authorizes access — a covering CIDR does not. */
static int ip_to_bin(const char *ip, int *fam, unsigned char *buf16);
static int host_decide(const ProxyContext *ctx, const char *host) {
    /* Try hostname rules (exact/suffix match) */
    if (host_match(ctx->host_rules, ctx->host_rule_count, host)) return 1;
    /* Try as a numeric IP against CIDR rules */
    int fam;
    unsigned char bin[16];
    if (ip_to_bin(host, &fam, bin) == 0) {
        /* Metadata carve-out: numeric IP in metadata range requires exact grant.
         * Also enforced in addr_permitted (resolve path); both paths required.
         * See egress-filter.md §4. */
        if (in_metadata_range(fam, bin))
            return granted_exact(ctx->cidr_rules, ctx->cidr_rule_count, fam, bin);
        if (cidr_match(ctx->cidr_rules, ctx->cidr_rule_count, fam, bin)) return 1;
    }
    return 0;
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

/* True if `ip` is in the blessed set and not expired. */
static int bless_contains(ProxyContext *ctx, const char *ip) {
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
            break;
        }
    }
    pthread_mutex_unlock(&ctx->blessed_mu);
    return found;
}

/* Add `ip` to the blessed set with a fresh TTL. Prunes expired entries and
 * evicts the oldest if the set is full. Refreshes the TTL on a duplicate. */
static void bless_add(ProxyContext *ctx, const char *ip) {
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
    pthread_mutex_unlock(&ctx->blessed_mu);
}

/* A resolved address is permitted per the egress-filter.md §4 pipeline:
 *   1. Metadata range → exact grant only (CIDR grants cannot reach metadata)
 *   2. Public IP → ALLOW
 *   3. Private IP in granted CIDR → ALLOW
 *   4. Otherwise → DENY
 * Ordering is security-critical: metadata check MUST precede CIDR check. */
static int addr_permitted(const ProxyContext *ctx, const char *ip) {
    int fam;
    unsigned char bin[16];
    if (ip_to_bin(ip, &fam, bin) != 0) return 0;

    /* Metadata range: only an exact literal grant authorizes access.
     * A covering CIDR (e.g. 169.254.0.0/16 or 0.0.0.0/0) is not enough.
     * Also enforced in host_decide (numeric CONNECT path); both paths required.
     * See egress-filter.md §4. */
    if (in_metadata_range(fam, bin))
        return granted_exact(ctx->cidr_rules, ctx->cidr_rule_count, fam, bin);

    /* Public IPs are always permitted (no private range match). */
    if (!http_is_private_ip(ip)) return 1;

    /* Private IP: allowed only if covered by a granted CIDR (includes exact
     * literal grants since those are stored as /32 or /128 rules). */
    return cidr_match(ctx->cidr_rules, ctx->cidr_rule_count, fam, bin);
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
        bless_add(ctx, ip);
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

/* Relay data bidirectionally between two fds until one closes. */
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
            write(client_fd, "ERROR denied\n", 13);
            close(client_fd);
            return;
        }
        char ip[INET6_ADDRSTRLEN];
        if (resolve_and_bless(ctx, host, ip, sizeof(ip)) == 0) {
            char resp[INET6_ADDRSTRLEN + 8];
            int n = snprintf(resp, sizeof(resp), "ADDR %s\n", ip);
            write(client_fd, resp, (size_t)n);
        } else {
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
    if (ip_to_bin(target, &fam, bin) == 0) {
        /* Numeric target: must be blessed by a prior RESOLVE, or an explicitly
         * granted literal IP. A literal IP that is neither (e.g. a raw
         * 1.2.3.4 SSRF attempt) is refused — there is no resolve to bless it. */
        if (bless_contains(ctx, target)) {
            dial = target;
        } else if (host_decide(ctx, target)) {
            bless_add(ctx, target);
            dial = target;
        } else {
            write(client_fd, "DENIED\n", 7);
            close(client_fd);
            return;
        }
    } else {
        /* Hostname target: allowlist, then resolve + SSRF-filter + bless. */
        if (!host_decide(ctx, target)) {
            write(client_fd, "DENIED\n", 7);
            close(client_fd);
            return;
        }
        if (resolve_and_bless(ctx, target, resolved, sizeof(resolved)) != 0) {
            write(client_fd, "ERROR resolve\n", 14);
            close(client_fd);
            return;
        }
        dial = resolved;
    }

    /* Bound simultaneous relays per call — refuse cleanly when at the cap. */
    if (!relay_acquire(ctx)) {
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

        /* Spawn per-connection thread (detached) */
        struct conn_args *ca = malloc(sizeof(*ca));
        if (!ca) { close(client); continue; }
        ca->ctx = ctx;
        ca->fd = client;

        pthread_mutex_lock(&ctx->blessed_mu);
        ctx->conn_active++;
        pthread_mutex_unlock(&ctx->blessed_mu);

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
               char **hosts, size_t host_count) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->listen_fd = -1;
    pthread_mutex_init(&ctx->blessed_mu, NULL);
    pthread_cond_init(&ctx->conn_cond, NULL);
    ctx->hosts = hosts;          /* borrowed — must outlive the proxy */
    ctx->host_count = host_count;

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

/* Convenience: bind + serve in one call (single-threaded callers and tests). */
int proxy_start(ProxyContext *ctx, const char *dir,
                char **hosts, size_t host_count) {
    if (proxy_bind(ctx, dir, hosts, host_count) != 0)
        return -1;
    if (proxy_serve(ctx) != 0) {
        proxy_stop(ctx);
        return -1;
    }
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
    ctx->hosts = NULL;          /* borrowed — not owned, do not free */
    ctx->host_count = 0;
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
