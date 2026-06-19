/* net_shim.c — loopback HTTP CONNECT proxy for static binaries.
 *
 * Static Go/Rust binaries bypass LD_PRELOAD, so they cannot use the preload
 * egress path. This tiny helper runs *inside* the sandbox netns, listens on a
 * loopback port advertised via HTTP_PROXY, accepts a single `CONNECT host:port`
 * per connection, and forwards "host:port\n" to the broker over the same
 * per-call UDS the preload uses. The broker is the sole policy authority — it
 * allowlist-checks, SSRF-filters, blesses, resolves, and dials. This binary
 * speaks no policy: it parses CONNECT, asks the broker, and splices bytes.
 *
 * It is a SEPARATE translation unit linking only libc — by construction it has
 * no access to the policy/resolver/secret/DB symbols, so it is structurally
 * incapable of making an egress decision (CONNECT-only; no absolute-form HTTP,
 * no DNS, no tunnelled-traffic inspection).
 *
 * argv: net_shim <listen_fd> <uds_path>
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

#define LINE_CAP   1024   /* max CONNECT request-line length (bounded) */
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
    int blanks = 0, guard = 0;
    char line[LINE_CAP];
    while (guard++ < 256) {
        int n = read_line(fd, line, sizeof(line));
        if (n <= 0) { (void)blanks; return; }  /* blank line or EOF: done */
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

/* Bidirectional byte splice until either side closes. No interpretation. */
static void splice_both(int c, int u) {
    struct pollfd fds[2] = { {c, POLLIN, 0}, {u, POLLIN, 0} };
    char buf[RELAY_BUF];
    for (;;) {
        if (poll(fds, 2, -1) <= 0) return;
        for (int i = 0; i < 2; i++) {
            if (fds[i].revents & (POLLIN | POLLHUP)) {
                ssize_t n = read(fds[i].fd, buf, sizeof(buf));
                if (n <= 0) return;
                int dst = (i == 0) ? u : c;
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

static void handle(int client, const char *uds) {
    char line[LINE_CAP];
    int n = read_line(client, line, sizeof(line));
    if (n <= 0) return;

    /* CONNECT-only: "CONNECT host:port HTTP/1.1". Anything else is refused —
     * absolute-form (plain-HTTP) proxying is intentionally unsupported. */
    if (strncmp(line, "CONNECT ", 8) != 0) {
        const char *r = "HTTP/1.1 405 Method Not Allowed\r\n\r\n";
        (void)!write(client, r, strlen(r));
        return;
    }
    char *hostport = line + 8;
    char *sp = strchr(hostport, ' ');
    if (sp) *sp = '\0';
    if (!hostport[0] || !strchr(hostport, ':')) {
        const char *r = "HTTP/1.1 400 Bad Request\r\n\r\n";
        (void)!write(client, r, strlen(r));
        return;
    }
    drain_headers(client);

    int up = uds_connect(uds);
    if (up < 0) {
        const char *r = "HTTP/1.1 502 Bad Gateway\r\n\r\n";
        (void)!write(client, r, strlen(r));
        return;
    }

    /* Ask the broker (its existing CONNECT preamble): "host:port\n". */
    char req[LINE_CAP + 2];
    int rn = snprintf(req, sizeof(req), "%s\n", hostport);
    if (rn > 0) (void)!write(up, req, (size_t)rn);

    char resp[64];
    int rp = read_line(up, resp, sizeof(resp));
    if (rp >= 2 && strncmp(resp, "OK", 2) == 0) {
        const char *ok = "HTTP/1.1 200 Connection established\r\n\r\n";
        if (write(client, ok, strlen(ok)) > 0)
            splice_both(client, up);
    } else {
        const char *r = "HTTP/1.1 403 Forbidden\r\n\r\n";
        (void)!write(client, r, strlen(r));
    }
    close(up);
}

int main(int argc, char **argv) {
    if (argc < 3) return 1;
    int lfd = atoi(argv[1]);
    const char *uds = argv[2];

    signal(SIGPIPE, SIG_IGN);
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
