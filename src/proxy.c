#define _GNU_SOURCE
#include "proxy.h"
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
#include <unistd.h>

#define PROXY_BACKLOG 8
#define RELAY_BUF_SIZE 4096
#define PREAMBLE_MAX 300

/* V83: check if host is in allowed list (case-insensitive).
 * Empty list = allow all. */
static int host_allowed(const ProxyContext *ctx, const char *host) {
    if (!ctx->allowed_hosts || ctx->allowed_count == 0)
        return 1;
    for (size_t i = 0; i < ctx->allowed_count; i++) {
        if (strcasecmp(host, ctx->allowed_hosts[i]) == 0)
            return 1;
    }
    return 0;
}

/* Resolve hostname to IPv4/IPv6 address string. Returns 0 on success. */
static int resolve_host(const char *host, char *ip_buf, size_t ip_cap) {
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0)
        return -1;
    int ok = -1;
    if (res->ai_family == AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)res->ai_addr;
        if (inet_ntop(AF_INET, &s->sin_addr, ip_buf, (socklen_t)ip_cap))
            ok = 0;
    } else if (res->ai_family == AF_INET6) {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)res->ai_addr;
        if (inet_ntop(AF_INET6, &s->sin6_addr, ip_buf, (socklen_t)ip_cap))
            ok = 0;
    }
    freeaddrinfo(res);
    return ok;
}

/* Open TCP connection to host:port. Returns fd or -1. */
static int tcp_connect(const char *host, uint16_t port) {
    struct addrinfo hints = {0}, *res, *rp;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0)
        return -1;
    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
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

/* Handle a single client connection from shell child. */
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
        if (resolve_host(host, ip, sizeof(ip)) == 0) {
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
    const char *host = preamble;
    uint16_t port = (uint16_t)atoi(colon + 1);

    /* V83: For IP-based connects, we need to check if the IP was resolved
     * by us (via RESOLVE). Since CLONE_NEWNET blocks direct connects anyway,
     * and the preload lib sends the IP it got from our RESOLVE, we allow
     * IPs that correspond to allowed hosts. For simplicity: if allowed_hosts
     * is set and host is an IP, check if it matches any resolved allowed host.
     * In practice, the preload lib sends IPs from our RESOLVE responses. */
    if (!host_allowed(ctx, host)) {
        /* Host might be an IP — allow if allowed_hosts is empty (allow-all mode) */
        if (ctx->allowed_hosts && ctx->allowed_count > 0) {
            write(client_fd, "DENIED\n", 7);
            close(client_fd);
            return;
        }
    }

    int remote_fd = tcp_connect(host, port);
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

int proxy_start(ProxyContext *ctx, const char *workspace,
                char **allowed_hosts, size_t allowed_count) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->listen_fd = -1;
    ctx->allowed_hosts = allowed_hosts;
    ctx->allowed_count = allowed_count;

    /* Build socket path */
    size_t wlen = strlen(workspace);
    size_t plen = wlen + sizeof("/.proxy.sock");
    ctx->sock_path = malloc(plen);
    if (!ctx->sock_path) return -1;
    snprintf(ctx->sock_path, plen, "%s/.proxy.sock", workspace);

    /* Remove stale socket */
    unlink(ctx->sock_path);

    /* Create UDS listener */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
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
    ctx->running = 1;

    /* Start accept loop thread (detached — dies with process) */
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t, &attr, proxy_loop, ctx);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        close(fd);
        unlink(ctx->sock_path);
        free(ctx->sock_path);
        ctx->sock_path = NULL;
        ctx->listen_fd = -1;
        return -1;
    }

    return 0;
}

void proxy_stop(ProxyContext *ctx) {
    ctx->running = 0;
    if (ctx->listen_fd >= 0) {
        close(ctx->listen_fd);
        ctx->listen_fd = -1;
    }
    if (ctx->sock_path) {
        unlink(ctx->sock_path);
        free(ctx->sock_path);
        ctx->sock_path = NULL;
    }
}

const char *proxy_sock_path(const ProxyContext *ctx) {
    return ctx->sock_path;
}
