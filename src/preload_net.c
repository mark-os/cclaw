/* libcclaw_net.so — LD_PRELOAD shared library for shell children (V82, V83).
 * Intercepts connect() and getaddrinfo() for AF_INET/AF_INET6 TCP,
 * routes through UDS to agent proxy thread at $CCLAW_PROXY_SOCK.
 * Graceful passthrough if proxy socket path unset or unreachable. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Real libc functions resolved via dlsym */
static int (*real_connect)(int, const struct sockaddr *, socklen_t);
static int (*real_getaddrinfo)(const char *, const char *,
                               const struct addrinfo *, struct addrinfo **);

static const char *proxy_sock_path;

__attribute__((constructor))
static void preload_init(void) {
    real_connect = dlsym(RTLD_NEXT, "connect");
    real_getaddrinfo = dlsym(RTLD_NEXT, "getaddrinfo");
    proxy_sock_path = getenv("CCLAW_PROXY_SOCK");
}

/* Open UDS connection to proxy. Returns fd or -1. */
static int open_proxy(void) {
    if (!proxy_sock_path || !proxy_sock_path[0])
        return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t plen = strlen(proxy_sock_path);
    if (plen >= sizeof(addr.sun_path)) {
        close(fd);
        return -1;
    }
    memcpy(addr.sun_path, proxy_sock_path, plen);

    if (real_connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Read a line from fd (up to max-1 bytes). Returns bytes read or -1. */
static int read_line(int fd, char *buf, int max) {
    int pos = 0;
    while (pos < max - 1) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n <= 0) return -1;
        buf[pos++] = c;
        if (c == '\n') break;
    }
    buf[pos] = '\0';
    return pos;
}

/* Extract host:port string from sockaddr. Returns 0 on success. */
static int addr_to_preamble(const struct sockaddr *addr, char *buf, int bufsz) {
    char host[256];
    uint16_t port;

    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in *sa4 = (const struct sockaddr_in *)addr;
        if (!inet_ntop(AF_INET, &sa4->sin_addr, host, sizeof(host)))
            return -1;
        port = ntohs(sa4->sin_port);
    } else if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6 *sa6 = (const struct sockaddr_in6 *)addr;
        if (!inet_ntop(AF_INET6, &sa6->sin6_addr, host, sizeof(host)))
            return -1;
        port = ntohs(sa6->sin6_port);
    } else {
        return -1;
    }

    int n = snprintf(buf, (size_t)bufsz, "%s:%u\n", host, port);
    if (n <= 0 || n >= bufsz) return -1;
    return 0;
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    if (!real_connect) {
        real_connect = dlsym(RTLD_NEXT, "connect");
        if (!real_connect) { errno = ENOSYS; return -1; }
    }

    /* Only intercept AF_INET/AF_INET6 TCP (SOCK_STREAM) */
    if (!addr || (addr->sa_family != AF_INET && addr->sa_family != AF_INET6))
        return real_connect(sockfd, addr, addrlen);

    int sock_type = 0;
    socklen_t optlen = sizeof(sock_type);
    if (getsockopt(sockfd, SOL_SOCKET, SO_TYPE, &sock_type, &optlen) != 0 ||
        sock_type != SOCK_STREAM)
        return real_connect(sockfd, addr, addrlen);

    /* Graceful passthrough if no proxy configured */
    int proxy_fd = open_proxy();
    if (proxy_fd < 0)
        return real_connect(sockfd, addr, addrlen);

    /* Send preamble: "host:port\n" */
    char preamble[280];
    if (addr_to_preamble(addr, preamble, sizeof(preamble)) != 0) {
        close(proxy_fd);
        return real_connect(sockfd, addr, addrlen);
    }

    size_t plen = strlen(preamble);
    if (write(proxy_fd, preamble, plen) != (ssize_t)plen) {
        close(proxy_fd);
        errno = ECONNREFUSED;
        return -1;
    }

    /* Read response */
    char resp[64];
    if (read_line(proxy_fd, resp, sizeof(resp)) <= 0) {
        close(proxy_fd);
        errno = ECONNREFUSED;
        return -1;
    }

    if (strncmp(resp, "OK\n", 3) == 0) {
        /* Proxy established connection — dup proxy fd onto our socket */
        dup2(proxy_fd, sockfd);
        close(proxy_fd);
        return 0;
    }

    close(proxy_fd);
    if (strncmp(resp, "DENIED\n", 7) == 0)
        errno = EACCES;
    else
        errno = ECONNREFUSED;
    return -1;
}

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
    if (!real_getaddrinfo) {
        real_getaddrinfo = dlsym(RTLD_NEXT, "getaddrinfo");
        if (!real_getaddrinfo) return EAI_SYSTEM;
    }

    /* Only intercept if proxy is available and this looks like a hostname resolve */
    if (!node || !proxy_sock_path || !proxy_sock_path[0])
        return real_getaddrinfo(node, service, hints, res);

    /* Send DNS resolve request to proxy: "RESOLVE <host>\n" */
    int proxy_fd = open_proxy();
    if (proxy_fd < 0)
        return real_getaddrinfo(node, service, hints, res);

    char req[300];
    int n = snprintf(req, sizeof(req), "RESOLVE %s\n", node);
    if (n <= 0 || n >= (int)sizeof(req) || write(proxy_fd, req, (size_t)n) != n) {
        close(proxy_fd);
        return real_getaddrinfo(node, service, hints, res);
    }

    /* Read response: "ADDR <ip>\n" or "ERROR ...\n" */
    char resp[280];
    if (read_line(proxy_fd, resp, sizeof(resp)) <= 0) {
        close(proxy_fd);
        return real_getaddrinfo(node, service, hints, res);
    }
    close(proxy_fd);

    if (strncmp(resp, "ADDR ", 5) != 0)
        return EAI_NONAME;

    /* Parse IP from response */
    char *ip = resp + 5;
    char *nl = strchr(ip, '\n');
    if (nl) *nl = '\0';

    /* Build addrinfo result */
    uint16_t port = 0;
    if (service) port = (uint16_t)atoi(service);

    struct sockaddr_in *sa4 = calloc(1, sizeof(*sa4));
    if (!sa4) return EAI_MEMORY;

    if (inet_pton(AF_INET, ip, &sa4->sin_addr) == 1) {
        sa4->sin_family = AF_INET;
        sa4->sin_port = htons(port);

        struct addrinfo *ai = calloc(1, sizeof(*ai));
        if (!ai) { free(sa4); return EAI_MEMORY; }
        ai->ai_family = AF_INET;
        ai->ai_socktype = SOCK_STREAM;
        ai->ai_protocol = 0;
        ai->ai_addrlen = sizeof(*sa4);
        ai->ai_addr = (struct sockaddr *)sa4;
        ai->ai_next = NULL;
        *res = ai;
        return 0;
    }

    /* Try IPv6 */
    free(sa4);
    struct sockaddr_in6 *sa6 = calloc(1, sizeof(*sa6));
    if (!sa6) return EAI_MEMORY;

    if (inet_pton(AF_INET6, ip, &sa6->sin6_addr) == 1) {
        sa6->sin6_family = AF_INET6;
        sa6->sin6_port = htons(port);

        struct addrinfo *ai = calloc(1, sizeof(*ai));
        if (!ai) { free(sa6); return EAI_MEMORY; }
        ai->ai_family = AF_INET6;
        ai->ai_socktype = SOCK_STREAM;
        ai->ai_protocol = 0;
        ai->ai_addrlen = sizeof(*sa6);
        ai->ai_addr = (struct sockaddr *)sa6;
        ai->ai_next = NULL;
        *res = ai;
        return 0;
    }

    free(sa6);
    return EAI_NONAME;
}

void freeaddrinfo(struct addrinfo *res) {
    /* Only free our custom-allocated results */
    while (res) {
        struct addrinfo *next = res->ai_next;
        free(res->ai_addr);
        free(res);
        res = next;
    }
}
