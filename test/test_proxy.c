/* test_proxy.c — unit test for credential proxy thread.
 * Tests UDS accept, RESOLVE, CONNECT preamble, allowed_hosts enforcement. */
#define _POSIX_C_SOURCE 200809L
#include "proxy.h"
#include "test_proxy_util.h"
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
#include <time.h>
#include <unistd.h>

static char tmpdir[128];

/* In-memory allowlist for "testbot" — the broker holds no DB handle, so the
 * proxy takes the host list as data (caps->hosts in production). A hostname, a
 * public literal IP, and a loopback-only hostname (to exercise the SSRF filter). */
static char *grant_hosts[] = {"example.com", "8.8.8.8", "localhost"};
#define GRANT_HOST_COUNT (sizeof(grant_hosts) / sizeof(grant_hosts[0]))

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

static void test_resolve_blessed(void) {
    /* A granted, public literal IP resolves and is blessed. getaddrinfo of a
     * numeric literal does not touch the network. */
    ProxyContext ctx;
    int rc = proxy_start(&ctx, tmpdir, grant_hosts, GRANT_HOST_COUNT);
    assert(rc == 0);

    int fd = uds_connect(proxy_sock_path(&ctx));
    assert(fd >= 0);

    const char *req = "RESOLVE 8.8.8.8\n";
    write(fd, req, strlen(req));

    char resp[128];
    int n = read_line(fd, resp, sizeof(resp));
    assert(n > 0);
    assert(strncmp(resp, "ADDR 8.8.8.8", 12) == 0);
    close(fd);

    proxy_stop(&ctx);
    printf("  PASS: resolve blessed (granted public IP)\n");
}

static void test_resolve_denied(void) {
    /* A host not in the grant set is denied by the allowlist (no resolve). */
    ProxyContext ctx;
    int rc = proxy_start(&ctx, tmpdir, grant_hosts, GRANT_HOST_COUNT);
    assert(rc == 0);

    int fd = uds_connect(proxy_sock_path(&ctx));
    assert(fd >= 0);

    const char *req = "RESOLVE notallowed.test\n";
    write(fd, req, strlen(req));

    char resp[128];
    int n = read_line(fd, resp, sizeof(resp));
    assert(n > 0);
    assert(strncmp(resp, "ERROR", 5) == 0);
    close(fd);

    proxy_stop(&ctx);
    printf("  PASS: resolve denied (not in allowlist)\n");
}

static void test_resolve_private_rejected(void) {
    /* "localhost" IS granted, but it resolves only to loopback (127.0.0.1/::1),
     * which is not itself explicitly granted — the SSRF filter rejects it so a
     * granted name cannot be rebound onto a host-internal address. */
    ProxyContext ctx;
    int rc = proxy_start(&ctx, tmpdir, grant_hosts, GRANT_HOST_COUNT);
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
    printf("  PASS: resolve private rejected (SSRF)\n");
}

static void test_connect_denied(void) {
    /* DB grants only "example.com" — connecting to other host denied */
    ProxyContext ctx;
    int rc = proxy_start(&ctx, tmpdir, grant_hosts, GRANT_HOST_COUNT);
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

static void test_connect_literal_private_ip_denied(void) {
    /* A raw literal private IP that was never RESOLVE'd and is not explicitly
     * granted is refused — the anti-SSRF rule for direct numeric connects to
     * private space. */
    ProxyContext ctx;
    int rc = proxy_start(&ctx, tmpdir, grant_hosts, GRANT_HOST_COUNT);
    assert(rc == 0);

    int fd = uds_connect(proxy_sock_path(&ctx));
    assert(fd >= 0);

    const char *req = "10.9.9.9:80\n";
    write(fd, req, strlen(req));

    char resp[128];
    int n = read_line(fd, resp, sizeof(resp));
    assert(n > 0);
    assert(strncmp(resp, "DENIED", 6) == 0);
    close(fd);

    proxy_stop(&ctx);
    printf("  PASS: connect literal private IP denied (unblessed)\n");
}

static void test_connect_literal_ungranted_public_ip_denied(void) {
    /* A raw literal public IP with no covering CIDR grant is DENIED — a
     * hostname grant (e.g. "8.8.8.8" or "example.com" in grant_hosts) never
     * authorizes an unrelated literal IP. host_decide's numeric branch is
     * deliberately NOT symmetric with addr_permitted (the RESOLVE-completion
     * check): addr_permitted only ever runs after a hostname already passed
     * host_match, so unconditionally allowing the resulting public IP is
     * spending a grant that was already earned. A raw literal skips that
     * vetting entirely, so it needs its own CIDR grant — otherwise any tool
     * call could reach an arbitrary public host by IP with zero grants,
     * defeating the allowlist. Exercised via RESOLVE rather than a numeric
     * CONNECT: both preambles route through the same host_decide() gate, but
     * RESOLVE on a numeric literal never leaves the process (getaddrinfo of a
     * literal is a pure parse, see test_resolve_blessed) whereas CONNECT
     * would hand the (unreachable, real) address to dial_ip()'s untimed
     * connect() — proxy_stop() waits for that thread to finish, so a real
     * dial here could block the test for minutes. Same policy code path, no
     * real-network risk. */
    ProxyContext ctx;
    int rc = proxy_start(&ctx, tmpdir, grant_hosts, GRANT_HOST_COUNT);
    assert(rc == 0);

    int fd = uds_connect(proxy_sock_path(&ctx));
    assert(fd >= 0);

    const char *req = "RESOLVE 1.2.3.4\n";
    write(fd, req, strlen(req));

    char resp[128];
    int n = read_line(fd, resp, sizeof(resp));
    assert(n > 0);
    assert(strncmp(resp, "ERROR denied", 12) == 0);
    close(fd);

    proxy_stop(&ctx);
    printf("  PASS: literal ungranted public IP denied (no allowlist bypass by IP)\n");
}

/* A throwaway loopback listener that accepts and holds connections open, so the
 * broker's dial succeeds and the relay stays live (occupying a relay slot). */
static int g_sink_fds[PROXY_MAX_RELAYS + 8];
static int g_sink_n;
static void *sink_accept(void *arg) {
    int lfd = *(int *)arg;
    for (;;) {
        int c = accept(lfd, NULL, NULL);
        if (c < 0) return NULL;
        if (g_sink_n < (int)(sizeof(g_sink_fds) / sizeof(g_sink_fds[0])))
            g_sink_fds[g_sink_n++] = c;  /* hold it open */
    }
}

static void test_relay_cap(void) {
    /* Grant the loopback literal so numeric CONNECTs to it dial directly (the
     * operator-opt-in path bypasses the SSRF filter). Open PROXY_MAX_RELAYS live
     * tunnels; the next must be refused cleanly without wedging the broker. */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    assert(lfd >= 0);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    assert(bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    assert(listen(lfd, 128) == 0);
    socklen_t al = sizeof(sa);
    assert(getsockname(lfd, (struct sockaddr *)&sa, &al) == 0);
    int sink_port = ntohs(sa.sin_port);
    pthread_t th;
    pthread_create(&th, NULL, sink_accept, &lfd);

    char *hosts[] = {"127.0.0.1"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 1) == 0);

    int clients[PROXY_MAX_RELAYS];
    for (int i = 0; i < PROXY_MAX_RELAYS; i++) {
        int fd = uds_connect(proxy_sock_path(&ctx));
        assert(fd >= 0);
        char req[64];
        int rn = snprintf(req, sizeof(req), "127.0.0.1:%d\n", sink_port);
        assert(write(fd, req, (size_t)rn) == rn);
        char resp[64];
        assert(read_line(fd, resp, sizeof(resp)) > 0);
        assert(strncmp(resp, "OK", 2) == 0);
        clients[i] = fd;  /* keep open → relay stays live */
    }

    /* One past the cap: refused cleanly (DENIED), broker still serving. */
    int over = uds_connect(proxy_sock_path(&ctx));
    assert(over >= 0);
    char req[64];
    int rn = snprintf(req, sizeof(req), "127.0.0.1:%d\n", sink_port);
    assert(write(over, req, (size_t)rn) == rn);
    char resp[64];
    assert(read_line(over, resp, sizeof(resp)) > 0);
    assert(strncmp(resp, "DENIED", 6) == 0);
    close(over);

    /* Free one slot and confirm a new tunnel is admitted again. */
    close(clients[0]);
    nanosleep(&(struct timespec){.tv_nsec = 50000000}, NULL);
    int again = uds_connect(proxy_sock_path(&ctx));
    assert(again >= 0);
    assert(write(again, req, (size_t)rn) == rn);
    assert(read_line(again, resp, sizeof(resp)) > 0);
    assert(strncmp(resp, "OK", 2) == 0);
    close(again);

    for (int i = 1; i < PROXY_MAX_RELAYS; i++) close(clients[i]);
    proxy_stop(&ctx);
    close(lfd);
    printf("  PASS: relay cap enforced (refuse past PROXY_MAX_RELAYS)\n");
}

static void test_pending_cap(void) {
    /* Occupy PROXY_MAX_PENDING conn_active slots by opening connections that
     * never send a preamble line — each conn_thread blocks in
     * proxy_read_line() before ever reaching host_decide/dial_ip, exactly
     * like a hostname whose DNS never responds. The next connection must be
     * refused cleanly (accept-then-close, no response line) without wedging
     * the broker; releasing a few slots lets the broker admit fresh work. */
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, grant_hosts, GRANT_HOST_COUNT) == 0);

    int pending[PROXY_MAX_PENDING];
    for (int i = 0; i < PROXY_MAX_PENDING; i++) {
        int fd = uds_connect(proxy_sock_path(&ctx));
        assert(fd >= 0);
        pending[i] = fd;  /* held open, no preamble written */
    }
    /* Give the accept loop a moment to drain its backlog and register all
     * conn_active slots before probing the cap. */
    nanosleep(&(struct timespec){.tv_nsec = 200000000}, NULL);

    /* One past the cap: silently dropped at the TCP/UDS level — accepted
     * then immediately closed, no DENIED/ERROR line to read. */
    int over = uds_connect(proxy_sock_path(&ctx));
    assert(over >= 0);
    char buf[8];
    ssize_t n = read(over, buf, sizeof(buf));
    assert(n == 0);
    close(over);

    /* Free some slots and confirm the broker still serves normally. */
    for (int i = 0; i < 10; i++) close(pending[i]);
    nanosleep(&(struct timespec){.tv_nsec = 200000000}, NULL);

    int again = uds_connect(proxy_sock_path(&ctx));
    assert(again >= 0);
    const char *req = "badpreamble\n";
    write(again, req, strlen(req));
    char resp[64];
    assert(read_line(again, resp, sizeof(resp)) > 0);
    assert(strncmp(resp, "ERROR", 5) == 0);
    close(again);

    for (int i = 10; i < PROXY_MAX_PENDING; i++) close(pending[i]);
    proxy_stop(&ctx);
    printf("  PASS: pending cap enforced (refuse past PROXY_MAX_PENDING)\n");
}

int main(void) {
    alarm(10);
    setup_tmpdir();

    printf("test_proxy:\n");
    test_proxy_start_stop();
    test_resolve_blessed();
    test_resolve_denied();
    test_resolve_private_rejected();
    test_connect_denied();
    test_connect_literal_private_ip_denied();
    test_connect_literal_ungranted_public_ip_denied();
    test_connect_bad_preamble();
    test_relay_cap();
    test_pending_cap();

    cleanup_tmpdir();
    printf("All proxy tests passed.\n");
    return 0;
}
