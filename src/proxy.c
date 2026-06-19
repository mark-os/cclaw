#define _GNU_SOURCE
#include "proxy.h"
#include "http_policy.h"
#include <sqlite3.h>
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

/* Check if host is allowed by querying the grants table. Deny-by-default:
 * no policy source (db_path/agent_name) means no network, not allow-all. An
 * empty grant set therefore denies every host. */
static int host_allowed(const ProxyContext *ctx, const char *host) {
    if (!ctx->db_path || !ctx->agent_name)
        return 0;
    sqlite3 *db;
    if (sqlite3_open_v2(ctx->db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        return 0;
    const char *sql =
        "SELECT 1 FROM grants WHERE agent_name=?1 AND kind='host'"
        " AND value=?2 AND (expires_at IS NULL OR expires_at > unixepoch()) LIMIT 1";
    sqlite3_stmt *stmt;
    int allowed = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, ctx->agent_name, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, host, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            allowed = 1;
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return allowed;
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

/* A resolved address may be blessed if it is public, or if its literal form is
 * itself explicitly granted — the operator opting into a private target. A
 * hostname that merely *resolves* to a private IP is rejected (SSRF / rebind). */
static int addr_permitted(const ProxyContext *ctx, const char *ip) {
    if (!http_is_private_ip(ip)) return 1;
    return host_allowed(ctx, ip);
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
        if (!host_allowed(ctx, host)) {
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
        } else if (host_allowed(ctx, target)) {
            bless_add(ctx, target);
            dial = target;
        } else {
            write(client_fd, "DENIED\n", 7);
            close(client_fd);
            return;
        }
    } else {
        /* Hostname target: allowlist, then resolve + SSRF-filter + bless. */
        if (!host_allowed(ctx, target)) {
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

    int remote_fd = dial_ip(dial, port);
    if (remote_fd < 0) {
        write(client_fd, "ERROR connect\n", 14);
        close(client_fd);
        return;
    }

    write(client_fd, "OK\n", 3);
    relay(client_fd, remote_fd);
    close(remote_fd);
    close(client_fd);
}

/* Per-connection thread */
struct conn_args {
    ProxyContext *ctx;
    int fd;
};

static void *conn_thread(void *arg) {
    struct conn_args *ca = (struct conn_args *)arg;
    handle_client(ca->ctx, ca->fd);
    free(ca);
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

        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &attr, conn_thread, ca) != 0) {
            close(client);
            free(ca);
        }
        pthread_attr_destroy(&attr);
    }

    return NULL;
}

/* Bind + listen on the per-call UDS, but do not start serving yet. Splitting
 * bind from serve lets the broker create the socket while still single-threaded,
 * fork the sandbox (a single-threaded fork — no fork-in-threaded-process
 * hazard), and only then start the accept thread. Returns 0 on success. */
int proxy_bind(ProxyContext *ctx, const char *workspace,
               const char *db_path, const char *agent_name) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->listen_fd = -1;
    pthread_mutex_init(&ctx->blessed_mu, NULL);
    ctx->db_path = db_path ? strdup(db_path) : NULL;
    ctx->agent_name = agent_name ? strdup(agent_name) : NULL;

    /* Per-call socket path: unique per process so concurrent brokers (each its
     * own pid) never collide. Sequential reuse within one process is covered by
     * the unlink below and proxy_stop's unlink. */
    size_t plen = strlen(workspace) + 32;
    ctx->sock_path = malloc(plen);
    if (!ctx->sock_path) return -1;
    snprintf(ctx->sock_path, plen, "%s/.proxy.%d.sock", workspace, (int)getpid());

    /* Remove stale socket */
    unlink(ctx->sock_path);

    /* Create UDS listener (CLOEXEC so it never rides into the sandbox exec) */
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { free(ctx->sock_path); ctx->sock_path = NULL; return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(ctx->sock_path) >= sizeof(addr.sun_path)) {
        close(fd);
        free(ctx->sock_path);
        ctx->sock_path = NULL;
        return -1;
    }
    strncpy(addr.sun_path, ctx->sock_path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        free(ctx->sock_path);
        ctx->sock_path = NULL;
        return -1;
    }

    if (listen(fd, PROXY_BACKLOG) != 0) {
        close(fd);
        unlink(ctx->sock_path);
        free(ctx->sock_path);
        ctx->sock_path = NULL;
        return -1;
    }

    ctx->listen_fd = fd;
    return 0;
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
int proxy_start(ProxyContext *ctx, const char *workspace,
                const char *db_path, const char *agent_name) {
    if (proxy_bind(ctx, workspace, db_path, agent_name) != 0)
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
    if (ctx->listen_fd >= 0) {
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
    }
    if (ctx->sock_path) {
        unlink(ctx->sock_path);
        free(ctx->sock_path);
        ctx->sock_path = NULL;
    }
    free(ctx->db_path);
    ctx->db_path = NULL;
    free(ctx->agent_name);
    ctx->agent_name = NULL;
    pthread_mutex_destroy(&ctx->blessed_mu);
}

const char *proxy_sock_path(const ProxyContext *ctx) {
    return ctx->sock_path;
}
