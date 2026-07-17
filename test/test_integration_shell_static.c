/* test_integration_shell_static.c — Stage 3: static-binary egress via the
 * in-sandbox loopback shim.
 *
 * Drives the whole path: the --run-tool broker stands up the per-call proxy,
 * the sandbox brings up lo and forks net_shim, the shim accepts an HTTP CONNECT
 * and forwards host:port to the broker over the same per-call UDS, the broker
 * (sole policy authority) allowlist-checks + dials a host-loopback mock.
 *
 * We approximate a static binary with `curl --proxytunnel` run with LD_PRELOAD
 * UNSET — so curl does not use the dynamic preload path and instead honors the
 * HTTP_PROXY the sandbox injected, exactly like a static client would. CONNECT
 * (not absolute-form) is forced with --proxytunnel since the shim is
 * CONNECT-only.
 *
 * ns-gated: skips when unprivileged user namespaces are unavailable.
 */
#define _GNU_SOURCE
#include "test_run_tool_shell.h"
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "test_util.h"

static char workspace[256];
static int ns_available = 1;

/* Broker allowlist: the loopback literal, so the broker permits the mock. */
static const char *allow_loopback[] = {"127.0.0.1"};

static void setup_workspace(void) {
    snprintf(workspace, sizeof(workspace), "/tmp/cclaw_static_%d", getpid());
    mkdir(workspace, 0755);
}

static void cleanup_workspace(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", workspace);
    (void)!system(cmd);
}

static int start_mock_tcp(uint16_t *out_port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(fd, 4) == 0);
    socklen_t alen = sizeof(addr);
    getsockname(fd, (struct sockaddr *)&addr, &alen);
    *out_port = ntohs(addr.sin_port);
    return fd;
}

typedef struct { int listen_fd; const char *response; } MockArg;

static void *mock_http_thread(void *arg) {
    MockArg *ma = (MockArg *)arg;
    struct timeval tv = {5, 0};
    setsockopt(ma->listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int client = accept(ma->listen_fd, NULL, NULL);
    if (client < 0) return NULL;
    char buf[2048];
    ssize_t total = 0;
    while (total < (ssize_t)sizeof(buf) - 1) {
        ssize_t n = read(client, buf + total, sizeof(buf) - 1 - (size_t)total);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    (void)!write(client, ma->response, strlen(ma->response));
    close(client);
    return NULL;
}

static void check_prerequisites(void) {
    struct stat st;
    if (stat("./build/libcclaw_net.so", &st) != 0 || stat("./build/net_shim", &st) != 0) {
        printf("  SKIP: libcclaw_net.so / net_shim not built\n");
        exit(0);
    }
    ns_available = run_tool_ns_available(workspace);
}

/* Allowed host tunnels through the shim and relays the upstream response. */
static void test_static_tunnel_allowed(void) {
    if (!ns_available) { printf("  SKIP test_static_tunnel_allowed (no namespaces)\n"); return; }

    uint16_t port;
    int listen_fd = start_mock_tcp(&port);
    static const char *RESP =
        "HTTP/1.1 200 OK\r\nContent-Length: 12\r\nConnection: close\r\n\r\nSHIM_WORKS!\n";
    MockArg ma = {.listen_fd = listen_fd, .response = RESP};
    pthread_t thr;
    pthread_create(&thr, NULL, mock_http_thread, &ma);

    /* No LD_PRELOAD (simulate static binary); clear NO_PROXY so the loopback
     * mock is proxied; --proxytunnel forces CONNECT; rely on the injected
     * $HTTP_PROXY for the proxy address (also proves it was injected). */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "unset LD_PRELOAD; export NO_PROXY= no_proxy=; "
        "curl -s --max-time 5 -x \"$HTTP_PROXY\" --proxytunnel "
        "http://127.0.0.1:%u/test", port);

    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = cmd;
    r.workspace = workspace;
    r.timeout = 15;
    r.sandbox = 1;
    r.host_rules = allow_loopback;
    r.host_count = 1;

    char *result = run_tool_shell(&r);
    pthread_join(thr, NULL);
    close(listen_fd);

    assert(result != NULL);
    if (strstr(result, "SHIM_WORKS!")) {
        printf("  PASS test_static_tunnel_allowed\n");
    } else if (strstr(result, "curl") && strstr(result, "not found")) {
        printf("  SKIP test_static_tunnel_allowed (curl unavailable in sandbox)\n");
    } else {
        printf("  FAIL test_static_tunnel_allowed\n  output: %s\n", result);
        free(result);
        assert(0 && "static tunnel failed");
    }
    free(result);
}

/* A non-allowlisted host is denied by the broker — the shim returns a non-200
 * and curl gets no upstream body. */
static void test_static_tunnel_denied(void) {
    if (!ns_available) { printf("  SKIP test_static_tunnel_denied (no namespaces)\n"); return; }

    uint16_t port;
    int listen_fd = start_mock_tcp(&port);  /* exists, but host name is not granted */

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "unset LD_PRELOAD; export NO_PROXY= no_proxy=; "
        "curl -s --max-time 5 -x \"$HTTP_PROXY\" --proxytunnel "
        "http://denied.test:%u/test", port);

    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = cmd;
    r.workspace = workspace;
    r.timeout = 15;
    r.sandbox = 1;
    r.host_rules = allow_loopback;  /* only 127.0.0.1 — denied.test is not granted */
    r.host_count = 1;

    char *result = run_tool_shell(&r);
    close(listen_fd);

    assert(result != NULL);
    if (strstr(result, "SHIM_WORKS!")) {
        printf("  FAIL test_static_tunnel_denied\n  output: %s\n", result);
        free(result);
        assert(0 && "broker should have denied unlisted host");
    }
    printf("  PASS test_static_tunnel_denied\n");
    free(result);
}

int main(void) {
    TEST_INIT();
    signal(SIGPIPE, SIG_IGN);
    printf("test_integration_shell_static (Stage 3):\n");
    setup_workspace();
    check_prerequisites();
    test_static_tunnel_allowed();
    test_static_tunnel_denied();
    cleanup_workspace();
    printf("All static-binary shim tests passed.\n");
    return 0;
}
