/* test_proxy.c — unit test for credential proxy thread (V83, V86).
 * Tests UDS accept, RESOLVE, CONNECT preamble, allowed_hosts enforcement. */
#define _POSIX_C_SOURCE 200809L
#include "proxy.h"
#include <arpa/inet.h>
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static char tmpdir[128];

static void setup_tmpdir(void) {
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/test_proxy_XXXXXX");
    assert(mkdtemp(tmpdir) != NULL);
}

static void cleanup_tmpdir(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
    system(cmd);
}

/* Connect to UDS and send a line, read response */
static int uds_connect(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int read_line(int fd, char *buf, int max) {
    int pos = 0;
    while (pos < max - 1) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n <= 0) return pos;
        buf[pos++] = c;
        if (c == '\n') break;
    }
    buf[pos] = '\0';
    return pos;
}

static void test_proxy_start_stop(void) {
    ProxyContext ctx;
    int rc = proxy_start(&ctx, tmpdir, NULL, 0);
    assert(rc == 0);
    assert(proxy_sock_path(&ctx) != NULL);

    /* Verify socket file exists */
    struct stat st;
    assert(stat(proxy_sock_path(&ctx), &st) == 0);
    assert(S_ISSOCK(st.st_mode));

    /* Verify we can connect */
    int fd = uds_connect(proxy_sock_path(&ctx));
    assert(fd >= 0);
    close(fd);

    proxy_stop(&ctx);

    /* Socket file should be removed */
    assert(stat(proxy_sock_path(&ctx), &st) != 0);
    printf("  PASS: proxy start/stop\n");
}

static void test_resolve_allowed(void) {
    /* No allowlist = allow all */
    ProxyContext ctx;
    int rc = proxy_start(&ctx, tmpdir, NULL, 0);
    assert(rc == 0);

    int fd = uds_connect(proxy_sock_path(&ctx));
    assert(fd >= 0);

    /* RESOLVE localhost should work */
    const char *req = "RESOLVE localhost\n";
    write(fd, req, strlen(req));

    char resp[128];
    int n = read_line(fd, resp, sizeof(resp));
    assert(n > 0);
    /* Should get "ADDR 127.0.0.1\n" or "ADDR ::1\n" */
    assert(strncmp(resp, "ADDR ", 5) == 0);
    close(fd);

    proxy_stop(&ctx);
    printf("  PASS: resolve allowed (no allowlist)\n");
}

static void test_resolve_denied(void) {
    /* Allowlist with only "example.com" — localhost should be denied */
    char *hosts[] = { "example.com" };
    ProxyContext ctx;
    int rc = proxy_start(&ctx, tmpdir, hosts, 1);
    assert(rc == 0);

    int fd = uds_connect(proxy_sock_path(&ctx));
    assert(fd >= 0);

    const char *req = "RESOLVE localhost\n";
    write(fd, req, strlen(req));

    char resp[128];
    int n = read_line(fd, resp, sizeof(resp));
    assert(n > 0);
    assert(strncmp(resp, "ERROR", 5) == 0);
    close(fd);

    proxy_stop(&ctx);
    printf("  PASS: resolve denied (not in allowlist)\n");
}

static void test_connect_denied(void) {
    /* Allowlist with only "example.com" — connecting to other host denied */
    char *hosts[] = { "example.com" };
    ProxyContext ctx;
    int rc = proxy_start(&ctx, tmpdir, hosts, 1);
    assert(rc == 0);

    int fd = uds_connect(proxy_sock_path(&ctx));
    assert(fd >= 0);

    const char *req = "evil.com:80\n";
    write(fd, req, strlen(req));

    char resp[128];
    int n = read_line(fd, resp, sizeof(resp));
    assert(n > 0);
    assert(strncmp(resp, "DENIED", 6) == 0);
    close(fd);

    proxy_stop(&ctx);
    printf("  PASS: connect denied (not in allowlist)\n");
}

static void test_connect_bad_preamble(void) {
    ProxyContext ctx;
    int rc = proxy_start(&ctx, tmpdir, NULL, 0);
    assert(rc == 0);

    int fd = uds_connect(proxy_sock_path(&ctx));
    assert(fd >= 0);

    /* No colon = bad preamble */
    const char *req = "badpreamble\n";
    write(fd, req, strlen(req));

    char resp[128];
    int n = read_line(fd, resp, sizeof(resp));
    assert(n > 0);
    assert(strncmp(resp, "ERROR", 5) == 0);
    close(fd);

    proxy_stop(&ctx);
    printf("  PASS: connect bad preamble\n");
}

int main(void) {
    alarm(10);
    setup_tmpdir();

    printf("test_proxy:\n");
    test_proxy_start_stop();
    test_resolve_allowed();
    test_resolve_denied();
    test_connect_denied();
    test_connect_bad_preamble();

    cleanup_tmpdir();
    printf("All proxy tests passed.\n");
    return 0;
}
