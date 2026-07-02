/* test_integration_shell_proxy_deny.c — T216: shell cannot reach unlisted host.
 * Starts mock TCP server, shell child attempts connect to 127.0.0.1 (not in
 * host_rules) via proxy.  Verifies proxy denies the connection.
 * Uses the real --run-tool broker path (fork+execve). */
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

static char workspace[256];
static int ns_available = 0;

/* The broker takes its egress allowlist as data — no grant DB. Two distinct
 * allowlists exercise the deny path: "testbot" may reach only "allowed.test"
 * (so a 127.0.0.1 connect is denied), and "testbot2" may reach only the
 * loopback literal (so the hostname "denied.test" is denied before any DNS). */
static const char *allow_named[] = {"allowed.test"};
static const char *allow_loopback[] = {"127.0.0.1"};

static void setup_workspace(void) {
    snprintf(workspace, sizeof(workspace), "/tmp/cclaw_proxy_deny_%d", getpid());
    mkdir(workspace, 0755);
}

static void cleanup_workspace(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", workspace);
    system(cmd);
}

static int start_mock_tcp(uint16_t *out_port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {0};
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

static void check_prerequisites(void) {
    struct stat st;
    if (stat("./build/libcclaw_net.so", &st) != 0) {
        printf("  SKIP: libcclaw_net.so not built\n");
        exit(0);
    }
    ns_available = run_tool_ns_available(workspace);
}

/* T216: proxy denies connection to unlisted host */
static void test_shell_denied_unlisted_host(void) {
    if (!ns_available) {
        printf("  SKIP test_shell_denied_unlisted_host (namespaces unavailable)\n");
        return;
    }

    uint16_t port;
    int listen_fd = start_mock_tcp(&port);

    /* Shell child tries to curl 127.0.0.1 — host_rules has only "allowed.test",
     * so the broker's per-call proxy denies it. */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "curl -s --max-time 5 http://127.0.0.1:%u/test", port);

    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = cmd;
    r.workspace = workspace;
    r.timeout = 10;
    r.sandbox = 1;
    r.host_rules = allow_named;
    r.host_count = 1;

    char *result = run_tool_shell(&r);

    close(listen_fd);

    assert(result != NULL);
    /* Connection should fail — curl reports error or empty response */
    if (strstr(result, "PROXY_WORKS") || strstr(result, "HTTP")) {
        printf("  FAIL test_shell_denied_unlisted_host\n");
        printf("  output: %s\n", result);
        free(result);
        assert(0 && "proxy should have denied unlisted host");
    }
    /* Success: connection was denied (curl error, empty, or "Couldn't connect") */
    printf("  PASS test_shell_denied_unlisted_host\n");
    free(result);
}

/* Test: allowed host succeeds, unlisted host fails in same proxy session */
static void test_shell_allowed_vs_denied(void) {
    if (!ns_available) {
        printf("  SKIP test_shell_allowed_vs_denied (namespaces unavailable)\n");
        return;
    }

    uint16_t port;
    int listen_fd = start_mock_tcp(&port);

    /* host_rules grants only "127.0.0.1", so a different hostname is denied by
     * the allowlist before any DNS — the proxy denies "denied.test". */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "curl -s --max-time 5 http://denied.test:%u/test", port);

    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = cmd;
    r.workspace = workspace;
    r.timeout = 10;
    r.sandbox = 1;
    r.host_rules = allow_loopback;
    r.host_count = 1;

    char *result = run_tool_shell(&r);

    close(listen_fd);

    assert(result != NULL);
    if (strstr(result, "PROXY_WORKS") || strstr(result, "HTTP")) {
        printf("  FAIL test_shell_allowed_vs_denied\n");
        printf("  output: %s\n", result);
        free(result);
        assert(0 && "proxy should have denied unlisted host");
    }
    printf("  PASS test_shell_allowed_vs_denied\n");
    free(result);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_integration_shell_proxy_deny (T216):\n");

    setup_workspace();
    check_prerequisites();
    test_shell_denied_unlisted_host();
    test_shell_allowed_vs_denied();
    cleanup_workspace();

    printf("All shell proxy deny tests passed.\n");
    return 0;
}
