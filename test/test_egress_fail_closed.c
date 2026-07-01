#define _DEFAULT_SOURCE
#include "sandbox.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Egress fail-closed: when the per-call proxy socket cannot be bound into the
 * sandbox (invalid/unbindable path), the child must be left NETWORKLESS —
 * CLONE_NEWNET with no egress path — never with direct network access. The
 * static-egress shim (sandbox_setup_static_egress) is best-effort by design;
 * this test pins down that "best-effort failure" degrades to no-network. */

static char workspace[256];

/* Child-side check, runs inside the sandbox namespace. Exit codes:
 * 0 = networkless as required, 50 = sandbox unavailable (skip),
 * 1..N = a connect unexpectedly succeeded / setup bug. */
static int child_assert_networkless(const char *proxy_sock) {
    SandboxConfig cfg = {0};
    cfg.workspace  = workspace;
    cfg.sandbox    = 1;
    cfg.net_mode   = 0;            /* proxy expected — but its path is bogus */
    cfg.env_mode   = 1;
    cfg.proxy_sock = proxy_sock;

    if (sandbox_child_setup(&cfg) != 0)
        return 50;                 /* no userns on this host — skip */

    /* Direct egress must be impossible: only lo exists in the fresh netns, so
     * a connect to an external address fails immediately (ENETUNREACH). */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 2;
    struct sockaddr_in ext;
    memset(&ext, 0, sizeof(ext));
    ext.sin_family = AF_INET;
    ext.sin_port = htons(80);
    inet_pton(AF_INET, "1.1.1.1", &ext.sin_addr);
    if (connect(fd, (struct sockaddr *)&ext, sizeof(ext)) == 0) {
        close(fd);
        return 1;                  /* direct network — fail-closed violated */
    }
    close(fd);

    /* If the shim advertised itself (HTTP_PROXY set), its UDS target was never
     * bound (bogus source path) — a CONNECT tunnel must not reach 200. */
    const char *proxy = getenv("HTTP_PROXY");
    if (proxy && strncmp(proxy, "http://127.0.0.1:", 17) == 0) {
        int port = atoi(proxy + 17);
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return 2;
        struct sockaddr_in lo;
        memset(&lo, 0, sizeof(lo));
        lo.sin_family = AF_INET;
        lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        lo.sin_port = htons((uint16_t)port);
        struct timeval tv = { .tv_sec = 3 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        if (connect(fd, (struct sockaddr *)&lo, sizeof(lo)) == 0) {
            const char *req = "CONNECT example.com:443 HTTP/1.1\r\n\r\n";
            (void)!write(fd, req, strlen(req));
            char resp[256] = {0};
            ssize_t n = read(fd, resp, sizeof(resp) - 1);
            if (n > 0 && strstr(resp, " 200 ")) {
                close(fd);
                return 3;          /* shim tunneled without a broker — bug */
            }
        }
        close(fd);
    }
    return 0;
}

static void run_case(const char *label, const char *proxy_sock) {
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0)
        _exit(child_assert_networkless(proxy_sock));

    int status = 0;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status));
    int code = WEXITSTATUS(status);
    if (code == 50) {
        printf("  SKIP %s (namespaces unavailable)\n", label);
        return;
    }
    if (code != 0) {
        printf("  FAIL %s (child exit %d)\n", label, code);
        assert(0);
    }
    printf("  PASS %s\n", label);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_egress_fail_closed:\n");

    snprintf(workspace, sizeof(workspace), "/tmp/cclaw_egress_fc_%d", getpid());
    mkdir(workspace, 0755);

    /* Nonexistent socket path: the in-sandbox bind mount finds no source */
    run_case("test_nonexistent_proxy_sock", "/nonexistent/dir/.proxy.sock");

    /* Path exists but is a directory — unbindable as a socket */
    run_case("test_unbindable_proxy_sock", "/tmp");

    char cmd[300];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", workspace);
    system(cmd);
    printf("  ALL PASSED\n");
    return 0;
}
