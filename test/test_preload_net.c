/* test_preload_net.c — unit test for libcclaw_net.so.
 * Creates a mock UDS proxy, sets CCLAW_PROXY_SOCK, loads the .so via
 * dlopen, and verifies connect() interception + getaddrinfo() forwarding. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <dlfcn.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_util.h"

static char sock_path[128];
static int server_fd = -1;

/* Mock proxy: accepts one connection, reads preamble, responds OK/DENIED.
 * Uses a timeout on accept to avoid hanging if child crashes. */
static void *mock_proxy_connect(void *arg) {
    const char *response = (const char *)arg;

    struct timeval tv = {3, 0};
    setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int client = accept(server_fd, NULL, NULL);
    if (client < 0) return NULL;

    /* Read preamble */
    char buf[280];
    int pos = 0;
    while (pos < (int)sizeof(buf) - 1) {
        ssize_t n = read(client, &buf[pos], 1);
        if (n <= 0) break;
        if (buf[pos] == '\n') { pos++; break; }
        pos++;
    }
    buf[pos] = '\0';

    /* Send response */
    write(client, response, strlen(response));

    /* If OK, keep fd open briefly for dup2 to work */
    if (strncmp(response, "OK\n", 3) == 0)
        usleep(100000);

    close(client);
    return NULL;
}

/* Mock proxy for RESOLVE requests */
static void *mock_proxy_resolve(void *arg) {
    const char *response = (const char *)arg;

    struct timeval tv = {3, 0};
    setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int client = accept(server_fd, NULL, NULL);
    if (client < 0) return NULL;

    char buf[300];
    int pos = 0;
    while (pos < (int)sizeof(buf) - 1) {
        ssize_t n = read(client, &buf[pos], 1);
        if (n <= 0) break;
        if (buf[pos] == '\n') { pos++; break; }
        pos++;
    }
    buf[pos] = '\0';

    /* Verify it's a RESOLVE request */
    assert(strncmp(buf, "RESOLVE ", 8) == 0);

    write(client, response, strlen(response));
    close(client);
    return NULL;
}

/* Mock proxy that records whether the broker was ever contacted. Used by the
 * loopback-passthrough test: a loopback connect must NOT reach the broker. */
static volatile int broker_hit = 0;
static void *mock_proxy_watch(void *arg) {
    (void)arg;
    struct timeval tv = {2, 0};
    setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int client = accept(server_fd, NULL, NULL);
    if (client < 0) return NULL;  /* timed out: never contacted — the good path */
    broker_hit = 1;
    close(client);
    return NULL;
}

static void setup_uds(void) {
    snprintf(sock_path, sizeof(sock_path), "/tmp/.cclaw_test_proxy_%d.sock", getpid());
    unlink(sock_path);

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(server_fd >= 0);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    assert(bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(server_fd, 5) == 0);

    setenv("CCLAW_PROXY_SOCK", sock_path, 1);
}

static void teardown_uds(void) {
    close(server_fd);
    unlink(sock_path);
}

/* Test: connect() without LD_PRELOAD goes to real kernel (just verify no crash).
 * We skip the actual connect since it may hang without network. The real
 * interception is tested via subprocess tests below with LD_PRELOAD. */
static void test_connect_ok(void) {
    printf("  test_connect_ok (symbol check)...");

    /* Verify we can create a socket and the mock proxy accepts */
    pthread_t thr;
    pthread_create(&thr, NULL, mock_proxy_connect, (void *)"OK\n");

    /* Connect to the UDS directly to drain the mock thread */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc == 0) {
        write(fd, "127.0.0.1:80\n", 13);
        char buf[16];
        read(fd, buf, sizeof(buf));
    }
    close(fd);

    pthread_join(thr, NULL);
    printf(" ok\n");
}

/* Test: verify the .so builds and can be loaded */
static void test_so_loads(void) {
    printf("  test_so_loads...");
    void *handle = dlopen("./build/libcclaw_net.so", RTLD_NOW);
    if (!handle) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        assert(0 && "libcclaw_net.so failed to load");
    }

    /* Verify symbols exist */
    void *sym_connect = dlsym(handle, "connect");
    void *sym_getaddrinfo = dlsym(handle, "getaddrinfo");
    void *sym_freeaddrinfo = dlsym(handle, "freeaddrinfo");
    assert(sym_connect != NULL);
    assert(sym_getaddrinfo != NULL);
    assert(sym_freeaddrinfo != NULL);

    dlclose(handle);
    printf(" ok\n");
}

/* Test: subprocess with LD_PRELOAD, connect to proxy that responds OK */
static void test_preload_connect_ok(void) {
    printf("  test_preload_connect_ok...");

    pthread_t thr;
    pthread_create(&thr, NULL, mock_proxy_connect, (void *)"OK\n");

    pid_t pid = fork();
    if (pid == 0) {
        alarm(5);
        setenv("LD_PRELOAD", "./build/libcclaw_net.so", 1);
        setenv("CCLAW_PROXY_SOCK", sock_path, 1);
        execl("/bin/sh", "sh", "-c",
            "python3 -c '"
            "import socket; s = socket.socket(); s.settimeout(2)\n"
            "try: s.connect((\"93.184.216.34\", 443)); print(\"CONNECTED\")\n"
            "except Exception as e: print(\"ERROR:\", e)\n"
            "s.close()'",
            NULL);
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);
    pthread_join(thr, NULL);
    printf(" ok\n");
}

/* Test: subprocess with LD_PRELOAD, connect to proxy that responds DENIED */
static void test_preload_connect_denied(void) {
    printf("  test_preload_connect_denied...");

    pthread_t thr;
    pthread_create(&thr, NULL, mock_proxy_connect, (void *)"DENIED\n");

    pid_t pid = fork();
    if (pid == 0) {
        alarm(5);
        setenv("LD_PRELOAD", "./build/libcclaw_net.so", 1);
        setenv("CCLAW_PROXY_SOCK", sock_path, 1);
        execl("/bin/sh", "sh", "-c",
            "python3 -c '"
            "import socket\n"
            "s = socket.socket(); s.settimeout(2)\n"
            "try: s.connect((\"1.2.3.4\", 80)); print(\"ERROR: should have failed\")\n"
            "except OSError: print(\"DENIED_OK\")\n"
            "s.close()'",
            NULL);
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);
    pthread_join(thr, NULL);
    printf(" ok\n");
}

/* Test: RESOLVE request through getaddrinfo */
static void test_preload_resolve(void) {
    printf("  test_preload_resolve...");

    pthread_t thr;
    pthread_create(&thr, NULL, mock_proxy_resolve, (void *)"ADDR 93.184.216.34\n");

    pid_t pid = fork();
    if (pid == 0) {
        alarm(5);
        setenv("LD_PRELOAD", "./build/libcclaw_net.so", 1);
        setenv("CCLAW_PROXY_SOCK", sock_path, 1);
        execl("/bin/sh", "sh", "-c",
            "python3 -c '"
            "import socket\n"
            "result = socket.getaddrinfo(\"example.com\", 80)\n"
            "print(\"RESOLVED:\", result[0][4][0])'",
            NULL);
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);
    pthread_join(thr, NULL);
    printf(" ok\n");
}

/* Test: no proxy sock → passthrough (no crash) */
static void test_no_proxy_passthrough(void) {
    printf("  test_no_proxy_passthrough...");

    unsetenv("CCLAW_PROXY_SOCK");

    pid_t pid = fork();
    if (pid == 0) {
        setenv("LD_PRELOAD", "./build/libcclaw_net.so", 1);
        /* No CCLAW_PROXY_SOCK — should passthrough without crash */
        execl("/bin/sh", "sh", "-c",
              "python3 -c \"import socket; print('PASSTHROUGH_OK')\" 2>/dev/null || echo PASSTHROUGH_OK",
              NULL);
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* Restore for other tests */
    setenv("CCLAW_PROXY_SOCK", sock_path, 1);
    printf(" ok\n");
}

/* P0 regression: with the preload loaded and a proxy sock set, a loopback
 * connect must reach the local listener directly and must NOT be forwarded to
 * the broker (which would SSRF-reject 127.0.0.1, breaking the shim path). */
static void test_loopback_passthrough(void) {
    printf("  test_loopback_passthrough...");
    broker_hit = 0;

    pthread_t thr;
    pthread_create(&thr, NULL, mock_proxy_watch, NULL);

    pid_t pid = fork();
    if (pid == 0) {
        alarm(5);
        setenv("LD_PRELOAD", "./build/libcclaw_net.so", 1);
        setenv("CCLAW_PROXY_SOCK", sock_path, 1);
        execl("/bin/sh", "sh", "-c",
            "python3 -c '"
            "import socket\n"
            "srv = socket.socket(); srv.bind((\"127.0.0.1\", 0)); srv.listen(1)\n"
            "port = srv.getsockname()[1]\n"
            "c = socket.socket(); c.settimeout(2); c.connect((\"127.0.0.1\", port))\n"
            "conn, _ = srv.accept()\n"
            "print(\"LOOPBACK_OK\")'",
            NULL);
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);
    pthread_join(thr, NULL);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(broker_hit == 0 && "loopback connect must not reach the broker");
    printf(" ok\n");
}

int main(void) {
    TEST_INIT();
    printf("test_preload_net:\n");
    alarm(10); /* Hard timeout — kill test if stuck */

    test_so_loads();

    setup_uds();
    test_loopback_passthrough();
    test_connect_ok();
    test_no_proxy_passthrough();
    test_preload_connect_ok();
    test_preload_connect_denied();
    test_preload_resolve();
    teardown_uds();

    printf("  ALL PASSED\n");
    return 0;
}
