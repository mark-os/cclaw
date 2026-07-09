/* test_net_shim.c — unit test for the loopback CONNECT shim (Stage 3).
 *
 * Namespace-free: net_shim only accept()s on a listener fd we pass it and talks
 * to a UDS we provide, so we can exercise the real binary directly. We stand up
 * a mock "broker" UDS that speaks the existing host:port/OK protocol, bind a
 * loopback TCP listener, hand both to net_shim, then drive HTTP CONNECT clients
 * against it and assert: allowed → 200 + bytes splice; denied → 407; non-CONNECT
 * → 405. Builds nothing real over the network — UDS + loopback only.
 *
 * Skips cleanly if ./build/net_shim is absent (make test does not build it).
 */
#define _GNU_SOURCE
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
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static char uds_path[128];
static int mock_listen_fd = -1;

/* Mock broker: accept a UDS conn, read "host:port\n". If host begins with
 * "deny" reply "DENIED\n"; else reply "OK\n", then echo one client chunk back
 * prefixed with "UP:" so the test can confirm the splice is bidirectional. */
static void *mock_conn(void *arg) {
    int fd = (int)(long)arg;
    char line[256];
    int pos = 0;
    while (pos < (int)sizeof(line) - 1) {
        char c;
        if (read(fd, &c, 1) <= 0) { close(fd); return NULL; }
        if (c == '\n') break;
        line[pos++] = c;
    }
    line[pos] = '\0';
    if (strncmp(line, "deny", 4) == 0) {
        (void)!write(fd, "DENIED\n", 7);
        close(fd);
        return NULL;
    }
    (void)!write(fd, "OK\n", 3);
    /* Tunnel established: read client's bytes, send a recognizable reply. */
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        char out[300];
        buf[n] = '\0';
        int on = snprintf(out, sizeof(out), "UP:%s", buf);
        (void)!write(fd, out, (size_t)on);
    }
    close(fd);
    return NULL;
}

static void *mock_broker(void *arg) {
    (void)arg;
    for (;;) {
        int c = accept(mock_listen_fd, NULL, NULL);
        if (c < 0) return NULL;  /* listener closed → shut down */
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_create(&t, &a, mock_conn, (void *)(long)c);
        pthread_attr_destroy(&a);
    }
}

static int start_mock_broker(void) {
    snprintf(uds_path, sizeof(uds_path), "/tmp/test_net_shim_%d.sock", getpid());
    unlink(uds_path);
    mock_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (mock_listen_fd < 0) return -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, uds_path, sizeof(a.sun_path) - 1);
    if (bind(mock_listen_fd, (struct sockaddr *)&a, sizeof(a)) != 0) return -1;
    if (listen(mock_listen_fd, 8) != 0) return -1;
    struct timeval tv = {5, 0};
    setsockopt(mock_listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return 0;
}

/* Bind a loopback TCP listener; return fd, set *port. */
static int bind_loopback(uint16_t *port) {
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
    assert(listen(fd, 8) == 0);
    socklen_t alen = sizeof(addr);
    getsockname(fd, (struct sockaddr *)&addr, &alen);
    *port = ntohs(addr.sin_port);
    return fd;
}

/* Connect to the shim's loopback port; caller drives HTTP CONNECT. */
static int shim_connect(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

static int read_some(int fd, char *buf, int cap) {
    int total = 0;
    while (total < cap - 1) {
        ssize_t n = read(fd, buf + total, (size_t)(cap - 1 - total));
        if (n <= 0) break;
        total += (int)n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;  /* got status + headers */
    }
    buf[total] = '\0';
    return total;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    alarm(20);
    signal(SIGPIPE, SIG_IGN);

    const char *exe = getenv("CCLAW_NET_SHIM_EXE");
    if (!exe) exe = "./build/net_shim";
    struct stat st;
    if (stat(exe, &st) != 0) {
        printf("test_net_shim: SKIP (%s not built)\n", exe);
        return 0;
    }

    assert(start_mock_broker() == 0);
    pthread_t broker_thr;
    pthread_create(&broker_thr, NULL, mock_broker, NULL);

    uint16_t port;
    int lfd = bind_loopback(&port);

    /* Fork+exec the real shim: net_shim <listen_fd> <uds_path>. */
    pid_t shim = fork();
    assert(shim >= 0);
    if (shim == 0) {
        char fdbuf[16];
        snprintf(fdbuf, sizeof(fdbuf), "%d", lfd);
        execl(exe, "net_shim", fdbuf, uds_path, (char *)NULL);
        _exit(127);
    }
    close(lfd);  /* the shim owns the listener now */

    printf("test_net_shim:\n");

    /* 1. Allowed CONNECT → 200 + bidirectional splice. */
    {
        int c = shim_connect(port);
        assert(c >= 0);
        const char *req = "CONNECT example.com:443 HTTP/1.1\r\nHost: example.com\r\n\r\n";
        assert(write(c, req, strlen(req)) > 0);
        char buf[512];
        read_some(c, buf, sizeof(buf));
        assert(strstr(buf, "HTTP/1.1 200") != NULL);
        /* Tunnel up: send a chunk, expect the mock's "UP:" echo back. */
        assert(write(c, "ping", 4) > 0);
        char relayed[256] = {0};
        ssize_t n = read(c, relayed, sizeof(relayed) - 1);
        assert(n > 0);
        relayed[n] = '\0';
        assert(strstr(relayed, "UP:ping") != NULL);
        close(c);
        printf("  PASS allowed CONNECT tunnels + splices\n");
    }

    /* 2. Denied CONNECT → 407 (proxy denied). */
    {
        int c = shim_connect(port);
        assert(c >= 0);
        const char *req = "CONNECT deny.example:443 HTTP/1.1\r\n\r\n";
        assert(write(c, req, strlen(req)) > 0);
        char buf[512];
        read_some(c, buf, sizeof(buf));
        assert(strstr(buf, "HTTP/1.1 407") != NULL);
        close(c);
        printf("  PASS denied CONNECT → 407\n");
    }

    /* 3. Non-CONNECT method → 405 (CONNECT-only). */
    {
        int c = shim_connect(port);
        assert(c >= 0);
        const char *req = "GET http://example.com/ HTTP/1.1\r\n\r\n";
        assert(write(c, req, strlen(req)) > 0);
        char buf[512];
        read_some(c, buf, sizeof(buf));
        assert(strstr(buf, "HTTP/1.1 405") != NULL);
        close(c);
        printf("  PASS non-CONNECT → 405\n");
    }

    /* Teardown */
    kill(shim, SIGKILL);
    waitpid(shim, NULL, 0);
    close(mock_listen_fd);  /* unblocks the broker accept loop */
    pthread_join(broker_thr, NULL);
    unlink(uds_path);

    printf("All net_shim tests passed.\n");
    return 0;
}
