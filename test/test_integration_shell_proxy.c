/* test_integration_shell_proxy.c — T215: shell curl through proxy.
 * Starts a mock TCP server, starts the proxy thread, runs a shell child
 * with LD_PRELOAD=libcclaw_net.so that connects through the proxy to the
 * mock server, verifies response relayed correctly.
 *
 * Key: the .so must be inside the workspace (accessible after pivot_root). */
#define _GNU_SOURCE
#include "proxy.h"
#include "tool_shell.h"
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
static int ns_available = 1;

/* The broker takes its egress allowlist as data (caps->hosts in production), so
 * the test passes it directly — no grant DB. Grant the loopback literal so the
 * proxy permits the mock server: a private IP is reachable only when explicitly
 * granted as a literal; a hostname resolving to loopback is still rejected. */
static char *allow_loopback[] = {"127.0.0.1"};

static void setup_workspace(void) {
    snprintf(workspace, sizeof(workspace), "/tmp/cclaw_proxy_integ_%d", getpid());
    mkdir(workspace, 0755);

    /* The per-call broker writes the preload .so from its embedded blob and
     * sets LD_PRELOAD/CCLAW_PROXY_SOCK itself — the test only supplies the
     * workspace, the allowlist, and the command. */
}

static void cleanup_workspace(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", workspace);
    system(cmd);
}

/* Start a TCP server on loopback, return listen fd + port */
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

/* Mock HTTP server thread data */
typedef struct {
    int listen_fd;
    const char *response;
} MockHttpArg;

static void *mock_http_thread(void *arg) {
    MockHttpArg *ma = (MockHttpArg *)arg;
    struct timeval tv = {5, 0};
    setsockopt(ma->listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int client = accept(ma->listen_fd, NULL, NULL);
    if (client < 0) return NULL;

    /* Drain HTTP request */
    char buf[2048];
    ssize_t total = 0;
    while (total < (ssize_t)sizeof(buf) - 1) {
        ssize_t n = read(client, buf + total, sizeof(buf) - 1 - (size_t)total);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }

    write(client, ma->response, strlen(ma->response));
    close(client);
    return NULL;
}

static void check_prerequisites(void) {
    struct stat st;
    if (stat("./build/libcclaw_net.so", &st) != 0) {
        printf("  SKIP: libcclaw_net.so not built\n");
        exit(0);
    }

    ShellConfig sc = {.timeout = 5, .workspace = workspace, .sandbox = 1};
    char *r = tool_shell_handler("{\"command\":\"echo ns_check\"}", &sc);
    if (r && strstr(r, "namespace sandbox unavailable")) {
        ns_available = 0;
    }
    free(r);
}

/* T215: shell child connects to mock TCP server through proxy */
static void test_shell_curl_through_proxy(void) {
    if (!ns_available) {
        printf("  SKIP test_shell_curl_through_proxy (namespaces unavailable)\n");
        return;
    }

    uint16_t port;
    int listen_fd = start_mock_tcp(&port);

    static const char *RESP =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 13\r\n"
        "Connection: close\r\n"
        "\r\n"
        "PROXY_WORKS!\n";

    MockHttpArg ma = { .listen_fd = listen_fd, .response = RESP };
    pthread_t thr;
    pthread_create(&thr, NULL, mock_http_thread, &ma);

    /* The broker stands up its own per-call proxy from workspace + grants;
     * the sandbox supplies LD_PRELOAD + CCLAW_PROXY_SOCK to the command. */
    char cmd_json[2048];
    snprintf(cmd_json, sizeof(cmd_json),
        "{\"command\":\"curl -s http://127.0.0.1:%u/test\"}", port);

    ShellConfig sc = {
        .timeout = 10,
        .workspace = workspace,
        .allowed_hosts = allow_loopback,
        .allowed_host_count = 1,
        .sandbox = 1,
    };
    char *result = tool_shell_handler(cmd_json, &sc);

    pthread_join(thr, NULL);
    close(listen_fd);

    assert(result != NULL);
    if (strstr(result, "PROXY_WORKS!")) {
        printf("  PASS test_shell_curl_through_proxy\n");
    } else if (strstr(result, "curl") && strstr(result, "not found")) {
        printf("  SKIP test_shell_curl_through_proxy (curl unavailable in sandbox)\n");
    } else {
        printf("  FAIL test_shell_curl_through_proxy\n");
        printf("  output: %s\n", result);
        free(result);
        assert(0 && "proxy relay failed");
    }
    free(result);
}

/* Test: verify response content integrity with multi-line payload */
static void test_proxy_relay_integrity(void) {
    if (!ns_available) {
        printf("  SKIP test_proxy_relay_integrity (namespaces unavailable)\n");
        return;
    }

    uint16_t port;
    int listen_fd = start_mock_tcp(&port);

    static const char RESP[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 36\r\n"
        "Connection: close\r\n"
        "\r\n"
        "line1:ABCDEF\nline2:123456\nline3:DONE\n";

    MockHttpArg ma = { .listen_fd = listen_fd, .response = RESP };
    pthread_t thr;
    pthread_create(&thr, NULL, mock_http_thread, &ma);

    char cmd_json[2048];
    snprintf(cmd_json, sizeof(cmd_json),
        "{\"command\":\"curl -s http://127.0.0.1:%u/\"}", port);

    ShellConfig sc = {
        .timeout = 10,
        .workspace = workspace,
        .allowed_hosts = allow_loopback,
        .allowed_host_count = 1,
        .sandbox = 1,
    };
    char *result = tool_shell_handler(cmd_json, &sc);

    pthread_join(thr, NULL);
    close(listen_fd);

    assert(result != NULL);
    if (strstr(result, "line1:ABCDEF") && strstr(result, "line2:123456") &&
        strstr(result, "line3:DONE")) {
        printf("  PASS test_proxy_relay_integrity\n");
    } else if (strstr(result, "curl") && strstr(result, "not found")) {
        printf("  SKIP test_proxy_relay_integrity (curl unavailable in sandbox)\n");
    } else {
        printf("  FAIL test_proxy_relay_integrity\n");
        printf("  output: %s\n", result);
        free(result);
        assert(0 && "integrity check failed");
    }
    free(result);
}

int main(void) {
    printf("test_integration_shell_proxy (T215):\n");

    setup_workspace();
    check_prerequisites();
    test_shell_curl_through_proxy();
    test_proxy_relay_integrity();
    cleanup_workspace();

    printf("All shell proxy integration tests passed.\n");
    return 0;
}
