/* net_shim.c — the in-sandbox end of the egress proxy.
 *
 * The sandbox's network namespace has no route out. This tiny helper runs
 * *inside* it and is the only way a program reaches the network: it turns a
 * request for "host:port" into the broker's one-line preamble on the per-call
 * UDS, and splices bytes. The broker is the sole policy authority — it
 * allowlist-checks, SSRF-filters, resolves, and dials. This binary speaks no
 * policy and no DNS, and inspects nothing it relays.
 *
 * It is a SEPARATE translation unit linking only libc — by construction it has
 * no access to the policy/resolver/secret/DB symbols, so it is structurally
 * incapable of making an egress decision.
 *
 * Two modes:
 *
 *   net_shim <listen_fd> <uds_path>
 *       An HTTP proxy on a loopback listener, advertised to the sandboxed
 *       command as HTTP_PROXY / HTTPS_PROXY. `CONNECT host:port` tunnels any
 *       TCP stream (HTTPS, WebSockets, gRPC — TLS stays end to end). A plain
 *       `GET http://host/path` is forwarded as one request to host:80.
 *
 *   net_shim connect <host> <port>      (UDS path from $CCLAW_PROXY_SOCK)
 *       The same tunnel on stdin/stdout, for programs that take a proxy
 *       *command* instead of proxy variables — `ssh -o ProxyCommand=...`.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define LINE_CAP   1024   /* max CONNECT request-line / header-line length */
#define REQ_CAP    8192   /* max request line for a plain-HTTP URL */
#define RELAY_BUF  4096

/* Read one CRLF-terminated line (CR stripped), capped. Returns length or -1. */
static int read_line(int fd, char *buf, int cap) {
    int pos = 0;
    while (pos < cap - 1) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n <= 0) return -1;
        if (c == '\n') break;
        if (c != '\r') buf[pos++] = c;
    }
    buf[pos] = '\0';
    return pos;
}

/* Consume request headers up to the blank line that ends them. Bounded so a
 * client that never sends the terminator cannot wedge the handler. */
static void drain_headers(int fd) {
    int guard = 0;
    char line[LINE_CAP];
    while (guard++ < 256) {
        int n = read_line(fd, line, sizeof(line));
        if (n <= 0) return;  /* blank line or EOF: done */
    }
}

static int uds_connect(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) { close(fd); return -1; }
    return fd;
}

/* Bidirectional byte splice until either side closes. No interpretation.
 * proxy.c's relay() is a near-identical loop — deliberately NOT shared; see
 * this file's header comment for why net_shim must stay a separate,
 * policy-free translation unit. */
static void splice_both(int cin, int cout, int u) {
    struct pollfd fds[2] = { {cin, POLLIN, 0}, {u, POLLIN, 0} };
    char buf[RELAY_BUF];
    for (;;) {
        if (poll(fds, 2, -1) <= 0) return;
        for (int i = 0; i < 2; i++) {
            if (fds[i].revents & (POLLIN | POLLHUP)) {
                ssize_t n = read(fds[i].fd, buf, sizeof(buf));
                if (n <= 0) return;
                int dst = (i == 0) ? u : cout;
                ssize_t off = 0;
                while (off < n) {
                    ssize_t w = write(dst, buf + off, (size_t)(n - off));
                    if (w <= 0) return;
                    off += w;
                }
            }
            if (fds[i].revents & (POLLERR | POLLNVAL)) return;
        }
    }
}

static void reply(int fd, const char *status, const char *body) {
    char r[LINE_CAP + 256];
    int n = snprintf(r, sizeof(r),
        "HTTP/1.1 %s\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n"
        "Connection: close\r\n\r\n%s", status, strlen(body), body);
    if (n >= (int)sizeof(r)) n = (int)sizeof(r) - 1;   /* truncated: send what fit */
    if (n > 0) (void)!write(fd, r, (size_t)n);
}

/* Ask the broker for host:port. Returns the connected UDS fd with the tunnel
 * open, or -1 with *denied set when the answer was a policy refusal (as
 * opposed to a name that did not resolve or a host that did not answer). */
static int broker_open(const char *uds, const char *hostport, int *denied) {
    *denied = 0;
    int up = uds_connect(uds);
    if (up < 0) return -1;
    char req[LINE_CAP + 2];
    int rn = snprintf(req, sizeof(req), "%s\n", hostport);
    if (rn <= 0 || rn >= (int)sizeof(req) || write(up, req, (size_t)rn) != rn) {
        close(up);
        return -1;
    }
    char resp[64];
    int rp = read_line(up, resp, sizeof(resp));
    if (rp >= 2 && strncmp(resp, "OK", 2) == 0) return up;
    *denied = (rp >= 6 && strncmp(resp, "DENIED", 6) == 0);
    close(up);
    return -1;
}

/* What a refused client is told. curl hides a failed CONNECT's body, so the
 * line the agent actually reads is the one the broker appends to the tool
 * result; this is for clients that do show it. */
static void reply_refused(int client, const char *hostport, int denied) {
    char body[LINE_CAP + 96];
    if (denied) {
        snprintf(body, sizeof(body),
                 "cclaw: egress to %s denied by policy (host not in the allowlist)\n",
                 hostport);
        reply(client, "403 Forbidden", body);
    } else {
        snprintf(body, sizeof(body), "cclaw: could not reach %s\n", hostport);
        reply(client, "502 Bad Gateway", body);
    }
}

static int ci_prefix(const char *s, int n, const char *lit) {
    int l = (int)strlen(lit);
    if (n < l) return 0;
    for (int i = 0; i < l; i++) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != lit[i]) return 0;
    }
    return 1;
}

/* Copy the client's request headers to the origin, minus the two that say how
 * long to keep the connection (ours, "Connection: close", is already sent).
 * Byte-wise with a small look-ahead, so a header of any length passes intact.
 * Returns 0 once the blank line has been forwarded, -1 on EOF or error. */
static int forward_headers(int client, int up) {
    char head[20];   /* long enough to recognise "proxy-connection:" */
    for (int lines = 0; lines < 512; lines++) {
        int n = 0, eol = 0;
        while (n < (int)sizeof(head) && !eol) {
            if (read(client, &head[n], 1) <= 0) return -1;
            eol = head[n++] == '\n';
        }
        int blank = eol && (n == 1 || (n == 2 && head[0] == '\r'));
        int drop = !blank && (ci_prefix(head, n, "connection:") ||
                              ci_prefix(head, n, "proxy-connection:"));
        if (!drop && write(up, head, (size_t)n) != n) return -1;
        while (!eol) {
            char c;
            if (read(client, &c, 1) <= 0) return -1;
            eol = c == '\n';
            if (!drop && write(up, &c, 1) != 1) return -1;
        }
        if (blank) return 0;
    }
    return -1;
}

/* Plain HTTP through the proxy: "GET http://host[:port]/path HTTP/1.1". The
 * request line is rewritten to origin form and the connection is pinned to
 * that one host: whatever else the client sends on it goes to the host the
 * broker decided on, never to another. "Connection: close" asks the origin
 * to end it after one exchange; the bytes are relayed until either side
 * closes. */
static void handle_absolute(int client, const char *uds, char *line) {
    char *sp1 = strchr(line, ' ');
    char *url = sp1 ? sp1 + 1 : NULL;
    char *sp2 = url ? strchr(url, ' ') : NULL;
    if (!sp2 || strncmp(url, "http://", 7) != 0) {
        reply(client, "400 Bad Request",
              "cclaw: this proxy takes CONNECT host:port or an http:// URL\n");
        return;
    }
    *sp1 = '\0';
    *sp2 = '\0';
    const char *method = line, *version = sp2 + 1;
    char *host = url + 7;
    char *path = strchr(host, '/');
    char hostport[LINE_CAP];
    int hl = path ? (int)(path - host) : (int)strlen(host);
    if (hl <= 0 || hl >= (int)sizeof(hostport) - 4 || memchr(host, '@', (size_t)hl)) {
        reply(client, "400 Bad Request", "cclaw: bad proxy request URL\n");
        return;
    }
    /* A port is the text after the last ':' that follows any IPv6 ']'. */
    const char *rb = memchr(host, ']', (size_t)hl);
    const char *scan = rb ? rb : host;
    int has_port = memchr(scan, ':', (size_t)(hl - (scan - host))) != NULL;
    snprintf(hostport, sizeof(hostport), "%.*s%s", hl, host, has_port ? "" : ":80");

    int denied;
    int up = broker_open(uds, hostport, &denied);
    if (up < 0) { reply_refused(client, hostport, denied); return; }

    char head[REQ_CAP + 64];
    int n = snprintf(head, sizeof(head), "%s %s %s\r\nConnection: close\r\n",
                     method, path ? path : "/", version);
    if (n > 0 && n < (int)sizeof(head) && write(up, head, (size_t)n) == n &&
        forward_headers(client, up) == 0)
        splice_both(client, client, up);
    close(up);
}

static void handle(int client, const char *uds) {
    char line[REQ_CAP];
    int n = read_line(client, line, sizeof(line));
    if (n <= 0) return;
    if (n >= REQ_CAP - 1) {
        reply(client, "414 URI Too Long", "cclaw: proxy request line too long\n");
        return;
    }

    if (strncmp(line, "CONNECT ", 8) != 0) {
        handle_absolute(client, uds, line);
        return;
    }
    char *hostport = line + 8;
    char *sp = strchr(hostport, ' ');
    if (sp) *sp = '\0';
    if (!hostport[0] || !strchr(hostport, ':')) {
        reply(client, "400 Bad Request", "cclaw: CONNECT needs host:port\n");
        return;
    }
    drain_headers(client);

    int denied;
    int up = broker_open(uds, hostport, &denied);
    if (up < 0) { reply_refused(client, hostport, denied); return; }

    const char *ok = "HTTP/1.1 200 Connection established\r\n\r\n";
    if (write(client, ok, strlen(ok)) > 0)
        splice_both(client, client, up);
    close(up);
}

/* `net_shim connect <host> <port>`: the tunnel on stdin/stdout. */
static int connect_mode(const char *host, const char *port) {
    const char *uds = getenv("CCLAW_PROXY_SOCK");
    char hostport[LINE_CAP];
    int n = snprintf(hostport, sizeof(hostport), "%s:%s", host, port);
    if (!uds || !uds[0] || n <= 0 || n >= (int)sizeof(hostport)) {
        fprintf(stderr, "cclaw: no egress proxy in this sandbox\n");
        return 1;
    }
    int denied;
    int up = broker_open(uds, hostport, &denied);
    if (up < 0) {
        fprintf(stderr, denied
            ? "cclaw: egress to %s denied by policy (host not in the allowlist)\n"
            : "cclaw: could not reach %s\n", hostport);
        return 1;
    }
    splice_both(STDIN_FILENO, STDOUT_FILENO, up);
    close(up);
    return 0;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc == 4 && strcmp(argv[1], "connect") == 0)
        return connect_mode(argv[2], argv[3]);
    if (argc < 3) return 1;
    int lfd = atoi(argv[1]);
    const char *uds = argv[2];

    signal(SIGCHLD, SIG_IGN);  /* auto-reap per-connection handlers */

    for (;;) {
        int client = accept(lfd, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            break;
        }
        /* Fork per connection so concurrent CONNECTs don't serialize. The
         * children are grandchildren of the command (PID 1); namespace teardown
         * on command exit SIGKILLs them. */
        pid_t p = fork();
        if (p == 0) {
            close(lfd);
            handle(client, uds);
            close(client);
            _exit(0);
        }
        close(client);
    }
    return 0;
}
