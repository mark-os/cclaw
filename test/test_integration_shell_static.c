/* test_integration_shell_static.c — sandboxed shell egress through the proxy
 * variables, the only mechanism there is.
 *
 * Drives the whole path: the --run-tool broker stands up the per-call proxy,
 * the sandbox brings up lo and forks net_shim, the shim takes the client's
 * request and forwards host:port to the broker over the per-call UDS, the
 * broker (sole policy authority) allowlist-checks + dials a host-loopback
 * mock. Covers CONNECT tunnels, plain-HTTP forwarding, what a denial looks
 * like to the client, and the stdin/stdout `connect` mode ssh uses.
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

typedef struct { int listen_fd; const char *response; char seen[2048]; } MockArg;

static void *mock_http_thread(void *arg) {
    MockArg *ma = (MockArg *)arg;
    struct timeval tv = {5, 0};
    setsockopt(ma->listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int client = accept(ma->listen_fd, NULL, NULL);
    if (client < 0) return NULL;
    char buf[2048] = "";
    ssize_t total = 0;
    while (total < (ssize_t)sizeof(buf) - 1) {
        ssize_t n = read(client, buf + total, sizeof(buf) - 1 - (size_t)total);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    memcpy(ma->seen, buf, (size_t)total + 1);
    (void)!write(client, ma->response, strlen(ma->response));
    close(client);
    return NULL;
}

static void check_prerequisites(void) {
    struct stat st;
    if (stat("./build/net_shim", &st) != 0) {
        printf("  SKIP: net_shim not built\n");
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

    /* Clear NO_PROXY so the loopback mock is proxied; --proxytunnel forces CONNECT; rely on the injected
     * $HTTP_PROXY for the proxy address (also proves it was injected). */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "export NO_PROXY= no_proxy=; "
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
        "export NO_PROXY= no_proxy=; "
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

static char *run_cmd(const char *cmd) {
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = cmd;
    r.workspace = workspace;
    r.timeout = 15;
    r.sandbox = 1;
    r.host_rules = allow_loopback;
    r.host_count = 1;
    char *result = run_tool_shell(&r);
    assert(result != NULL);
    return result;
}

static int no_curl(const char *result) {
    return strstr(result, "curl") && strstr(result, "not found");
}

/* A plain http:// URL with no --proxytunnel: the client sends the absolute
 * form, the shim forwards it as one origin-form request. */
static void test_plain_http_forwarded(void) {
    if (!ns_available) { printf("  SKIP test_plain_http_forwarded (no namespaces)\n"); return; }

    uint16_t port;
    int listen_fd = start_mock_tcp(&port);
    static const char *RESP =
        "HTTP/1.1 200 OK\r\nContent-Length: 12\r\nConnection: close\r\n\r\nPLAIN_WORKS\n";
    MockArg ma = {.listen_fd = listen_fd, .response = RESP};
    pthread_t thr;
    pthread_create(&thr, NULL, mock_http_thread, &ma);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "export NO_PROXY= no_proxy=; "
        "curl -s --max-time 5 -H 'X-Probe: kept' 'http://127.0.0.1:%u/a/b?q=1'", port);
    char *result = run_cmd(cmd);
    pthread_join(thr, NULL);
    close(listen_fd);

    if (no_curl(result)) {
        printf("  SKIP test_plain_http_forwarded (curl unavailable in sandbox)\n");
        free(result);
        return;
    }
    if (!strstr(result, "PLAIN_WORKS")) {
        printf("  FAIL test_plain_http_forwarded\n  output: %s\n  origin saw: %s\n",
               result, ma.seen);
        assert(0 && "plain http through the proxy failed");
    }
    /* The origin sees an ordinary request: origin-form line, the client's own
     * headers, one Connection header and it is ours. */
    assert(strncmp(ma.seen, "GET /a/b?q=1 HTTP/1.1\r\n", 23) == 0);
    assert(strstr(ma.seen, "X-Probe: kept\r\n"));
    assert(strstr(ma.seen, "Connection: close\r\n"));
    assert(!strstr(ma.seen, "Proxy-Connection"));
    assert(!strstr(ma.seen, "keep-alive"));
    printf("  PASS test_plain_http_forwarded\n");
    free(result);
}

/* A denied host answers 403 with the reason in the body, and the broker's
 * summary names the host in the tool result — for http as for https. */
static void test_denial_is_readable(void) {
    if (!ns_available) { printf("  SKIP test_denial_is_readable (no namespaces)\n"); return; }

    char *result = run_cmd("curl -s -i --max-time 5 http://denied.test/x");
    if (no_curl(result)) {
        printf("  SKIP test_denial_is_readable (curl unavailable in sandbox)\n");
        free(result);
        return;
    }
    if (!strstr(result, "403 Forbidden") ||
        !strstr(result, "egress to denied.test:80 denied by policy") ||
        !strstr(result, "proxy blocked host(s) not in allowlist: denied.test")) {
        printf("  FAIL test_denial_is_readable\n  output: %s\n", result);
        assert(0 && "denial not reported");
    }
    free(result);

    result = run_cmd("curl -sS --max-time 5 https://denied.test/x");
    assert(strstr(result, "403"));
    assert(strstr(result, "proxy blocked host(s) not in allowlist: denied.test"));
    printf("  PASS test_denial_is_readable\n");
    free(result);
}

/* Nothing is injected into the host's programs any more, and both spellings
 * of the proxy variables are set. */
static void test_env_is_proxy_only(void) {
    if (!ns_available) { printf("  SKIP test_env_is_proxy_only (no namespaces)\n"); return; }

    char *result = run_cmd(
        "echo \"pre=[$LD_PRELOAD]\"; "
        "[ -n \"$HTTPS_PROXY\" ] && [ \"$HTTPS_PROXY\" = \"$https_proxy\" ] && "
        "[ \"$HTTP_PROXY\" = \"$http_proxy\" ] && echo vars=ok; "
        "echo \"git=[$GIT_SSH_COMMAND]\"");
    if (!strstr(result, "pre=[]") || !strstr(result, "vars=ok") ||
        !strstr(result, "ProxyCommand='/.cclaw_connect connect %h %p'")) {
        printf("  FAIL test_env_is_proxy_only\n  output: %s\n", result);
        assert(0 && "sandbox env wrong");
    }
    printf("  PASS test_env_is_proxy_only\n");
    free(result);
}

/* `connect` mode: the tunnel on stdin/stdout, as ssh's ProxyCommand runs it. */
static void test_connect_mode(void) {
    if (!ns_available) { printf("  SKIP test_connect_mode (no namespaces)\n"); return; }

    uint16_t port;
    int listen_fd = start_mock_tcp(&port);
    static const char *RESP = "HTTP/1.0 200 OK\r\n\r\nCONNECT_MODE_WORKS\n";
    MockArg ma = {.listen_fd = listen_fd, .response = RESP};
    pthread_t thr;
    pthread_create(&thr, NULL, mock_http_thread, &ma);

    /* The sleep holds stdin open until the reply has come back. */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "{ printf 'GET /raw HTTP/1.0\\r\\n\\r\\n'; sleep 2; } | $CCLAW_CONNECT 127.0.0.1 %u",
        port);
    char *result = run_cmd(cmd);
    pthread_join(thr, NULL);
    close(listen_fd);
    if (!strstr(result, "CONNECT_MODE_WORKS") || strncmp(ma.seen, "GET /raw ", 9) != 0) {
        printf("  FAIL test_connect_mode\n  output: %s\n  origin saw: %s\n", result, ma.seen);
        assert(0 && "connect mode failed");
    }
    free(result);

    result = run_cmd("$CCLAW_CONNECT denied.test 22 </dev/null; echo rc=$?");
    if (!strstr(result, "egress to denied.test:22 denied by policy") ||
        !strstr(result, "rc=1") ||
        !strstr(result, "proxy blocked host(s) not in allowlist: denied.test")) {
        printf("  FAIL test_connect_mode (denied)\n  output: %s\n", result);
        assert(0 && "connect mode denial not reported");
    }
    printf("  PASS test_connect_mode\n");
    free(result);
}

int main(void) {
    TEST_INIT();
    signal(SIGPIPE, SIG_IGN);
    printf("test_integration_shell_static:\n");
    setup_workspace();
    check_prerequisites();
    test_static_tunnel_allowed();
    test_static_tunnel_denied();
    test_plain_http_forwarded();
    test_denial_is_readable();
    test_env_is_proxy_only();
    test_connect_mode();
    cleanup_workspace();
    printf("All shell proxy-variable tests passed.\n");
    return 0;
}
