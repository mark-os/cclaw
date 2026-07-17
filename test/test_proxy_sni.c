/* test_proxy_sni.c — unit test for the SNI-aware relay check (Q6):
 * a hostname-rule-authorized numeric-CONNECT relay is only as trustworthy as
 * the SNI it actually presents once the IP is shared across vhosts (shared-IP
 * CDN bypass). Builds hand-rolled ClientHello bytes; no real network beyond
 * loopback. */
#define _POSIX_C_SOURCE 200809L
#include "proxy.h"
#include "test_proxy_util.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static char tmpdir[128];

static void setup_tmpdir(void) {
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/test_proxy_sni_XXXXXX");
    assert(mkdtemp(tmpdir) != NULL);
}

static void cleanup_tmpdir(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
    system(cmd);
}

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

static void put_u16(unsigned char *p, size_t v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
static void put_u24(unsigned char *p, size_t v) {
    p[0] = (unsigned char)(v >> 16); p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)v;
}

/* Build a minimal single-record TLS ClientHello with an SNI server_name
 * extension for `sni`. Returns total length. */
static size_t build_client_hello(const char *sni, unsigned char *out, size_t cap) {
    size_t sni_len = strlen(sni);
    /* server_name extension: name_type(1) + name_len(2) + name */
    size_t entry_len = 1 + 2 + sni_len;
    /* server_name_list: list_len(2) + entry */
    size_t list_len = 2 + entry_len;
    /* extension: type(2) + len(2) + server_name_list */
    size_t ext_len = 4 + list_len;
    /* body: version(2)+random(32)+session_id_len(1)+ciphers(2+2)+comp(1+1)+ext_total_len(2)+ext */
    size_t body_len = 2 + 32 + 1 + (2 + 2) + (1 + 1) + 2 + ext_len;
    size_t hs_len = 4 + body_len;      /* handshake header + body */
    size_t rec_len = 5 + hs_len;       /* record header + handshake */
    assert(rec_len <= cap);

    unsigned char *p = out;
    *p++ = 0x16; *p++ = 0x03; *p++ = 0x03;         /* record: handshake, TLS 1.2 */
    put_u16(p, hs_len);                             /* record length = handshake msg */
    p += 2;

    *p++ = 0x01;                                    /* handshake type: ClientHello */
    put_u24(p, body_len); p += 3;

    *p++ = 0x03; *p++ = 0x03;                       /* client_version TLS 1.2 */
    memset(p, 0, 32); p += 32;                      /* random */
    *p++ = 0x00;                                    /* session_id_len = 0 */
    put_u16(p, 2); p += 2;                          /* cipher_suites_len = 2 */
    *p++ = 0x00; *p++ = 0x2f;                       /* one cipher suite */
    *p++ = 0x01;                                    /* compression_methods_len = 1 */
    *p++ = 0x00;                                    /* null compression */

    put_u16(p, ext_len); p += 2;                    /* extensions total length */
    put_u16(p, 0x0000); p += 2;                     /* extension type: server_name */
    put_u16(p, list_len); p += 2;                   /* extension length */
    put_u16(p, entry_len); p += 2;                  /* server_name_list length */
    *p++ = 0x00;                                    /* name_type: host_name */
    put_u16(p, sni_len); p += 2;                    /* name length */
    memcpy(p, sni, sni_len); p += sni_len;

    return (size_t)(p - out);
}

/* Loopback "remote" sink: accepts one connection, records whatever bytes
 * arrive (or none, if the proxy refuses the relay before splicing). */
static int sink_listen(int *port_out) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    assert(lfd >= 0);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    assert(bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    assert(listen(lfd, 8) == 0);
    socklen_t al = sizeof(sa);
    assert(getsockname(lfd, (struct sockaddr *)&sa, &al) == 0);
    *port_out = ntohs(sa.sin_port);
    return lfd;
}

struct sink_result {
    int accepted;    /* 1 if a connection was accepted */
    int got_bytes;   /* 1 if any bytes were read after accept */
};

struct sink_arg {
    int lfd;
    struct sink_result *res;
};

static void *sink_once(void *arg) {
    struct sink_arg *sa = (struct sink_arg *)arg;
    /* Testing rule: listener must time out rather than block forever if the
     * client side dies before connecting. */
    struct timeval ltv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(sa->lfd, SOL_SOCKET, SO_RCVTIMEO, &ltv, sizeof(ltv));
    int c = accept(sa->lfd, NULL, NULL);
    if (c < 0) return NULL;
    sa->res->accepted = 1;
    char buf[64];
    /* Give the proxy a moment to either relay or refuse, then check. */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = read(c, buf, sizeof(buf));
    sa->res->got_bytes = (n > 0);
    close(c);
    return NULL;
}

static void test_sni_mismatch_blocked(void) {
    /* Grant only "good.example.com" (host rule) + the loopback CIDR (needed
     * for addr_permitted to accept the private resolved address). "localhost"
     * resolves to loopback via the real system resolver without touching the
     * network, giving us a hostname-target CONNECT (via_hostname=1) that
     * dials our sink. The ClientHello's SNI ("evil.example.com") does not
     * match any granted host rule, so the relay must be refused. */
    int port;
    int lfd = sink_listen(&port);
    struct sink_result res = {0, 0};
    struct sink_arg sarg = { lfd, &res };
    pthread_t th;
    pthread_create(&th, NULL, sink_once, &sarg);

    char *hosts[] = {"localhost", "good.example.com", "127.0.0.0/8"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 3) == 0);

    int fd = uds_connect(proxy_sock_path(&ctx));
    assert(fd >= 0);
    char req[64];
    int rn = snprintf(req, sizeof(req), "localhost:%d\n", port);
    assert(write(fd, req, (size_t)rn) == rn);

    char resp[64];
    int n = read_line(fd, resp, sizeof(resp));
    assert(n > 0);
    assert(strncmp(resp, "OK", 2) == 0);

    unsigned char hello[512];
    size_t hlen = build_client_hello("evil.example.com", hello, sizeof(hello));
    write(fd, hello, hlen);

    /* Proxy should close the client side without relaying. A clean EOF (0) or
     * ECONNRESET (the Linux AF_UNIX quirk when the peer closes with our
     * unread ClientHello bytes still queued) both mean "closed, no relay". */
    char buf[16];
    ssize_t rd = read(fd, buf, sizeof(buf));
    assert(rd == 0 || (rd < 0 && errno == ECONNRESET));
    close(fd);

    pthread_join(th, NULL);
    assert(res.accepted == 1);
    assert(res.got_bytes == 0);   /* refused before any relay */

    proxy_stop(&ctx);
    close(lfd);
    printf("  PASS: SNI mismatch blocked (shared-IP CDN vhost hop refused)\n");
}

static void test_sni_match_allowed(void) {
    /* Same setup, but the ClientHello's SNI is "localhost" — matches the
     * granted hostname rule that authorized this connection. Relay proceeds
     * normally. */
    int port;
    int lfd = sink_listen(&port);
    struct sink_result res = {0, 0};
    struct sink_arg sarg = { lfd, &res };
    pthread_t th;
    pthread_create(&th, NULL, sink_once, &sarg);

    char *hosts[] = {"localhost", "127.0.0.0/8"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 2) == 0);

    int fd = uds_connect(proxy_sock_path(&ctx));
    assert(fd >= 0);
    char req[64];
    int rn = snprintf(req, sizeof(req), "localhost:%d\n", port);
    assert(write(fd, req, (size_t)rn) == rn);

    char resp[64];
    int n = read_line(fd, resp, sizeof(resp));
    assert(n > 0);
    assert(strncmp(resp, "OK", 2) == 0);

    unsigned char hello[512];
    size_t hlen = build_client_hello("localhost", hello, sizeof(hello));
    assert(write(fd, hello, hlen) == (ssize_t)hlen);

    pthread_join(th, NULL);
    assert(res.accepted == 1);
    assert(res.got_bytes == 1);   /* relayed through to the sink */

    close(fd);
    proxy_stop(&ctx);
    close(lfd);
    printf("  PASS: SNI match allowed (relay proceeds)\n");
}

static void test_sni_absent_failopen(void) {
    /* Non-TLS bytes (plain HTTP) on a hostname-authorized connection —
     * sni_check() must fail open immediately (byte[0] != 0x16) since a
     * hostname grant doesn't imply the protocol is HTTPS. */
    int port;
    int lfd = sink_listen(&port);
    struct sink_result res = {0, 0};
    struct sink_arg sarg = { lfd, &res };
    pthread_t th;
    pthread_create(&th, NULL, sink_once, &sarg);

    char *hosts[] = {"localhost", "127.0.0.0/8"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 2) == 0);

    int fd = uds_connect(proxy_sock_path(&ctx));
    assert(fd >= 0);
    char req[64];
    int rn = snprintf(req, sizeof(req), "localhost:%d\n", port);
    assert(write(fd, req, (size_t)rn) == rn);

    char resp[64];
    int n = read_line(fd, resp, sizeof(resp));
    assert(n > 0);
    assert(strncmp(resp, "OK", 2) == 0);

    const char *plain = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
    assert(write(fd, plain, strlen(plain)) == (ssize_t)strlen(plain));

    pthread_join(th, NULL);
    assert(res.accepted == 1);
    assert(res.got_bytes == 1);   /* fail-open: relayed through */

    close(fd);
    proxy_stop(&ctx);
    close(lfd);
    printf("  PASS: non-TLS bytes fail open (relay proceeds)\n");
}

/* RESOLVE the hostname first (blessing its IP via-hostname), then CONNECT to
 * the returned numeric IP — the common getaddrinfo-shim path. Returns the
 * connected fd after the OK, ready for client bytes. */
static int numeric_connect_after_resolve(ProxyContext *ctx, int port) {
    int rfd = uds_connect(proxy_sock_path(ctx));
    assert(rfd >= 0);
    const char *rq = "RESOLVE localhost\n";
    assert(write(rfd, rq, strlen(rq)) == (ssize_t)strlen(rq));
    char resp[64];
    int n = read_line(rfd, resp, sizeof(resp));
    close(rfd);
    assert(n > 0);
    assert(strncmp(resp, "ADDR 127.0.0.1", 14) == 0);

    int fd = uds_connect(proxy_sock_path(ctx));
    assert(fd >= 0);
    char req[64];
    int rn = snprintf(req, sizeof(req), "127.0.0.1:%d\n", port);
    assert(write(fd, req, (size_t)rn) == rn);
    n = read_line(fd, resp, sizeof(resp));
    assert(n > 0);
    assert(strncmp(resp, "OK", 2) == 0);
    return fd;
}

static void test_sni_numeric_blessed_gated(void) {
    /* The gap this closes (review-2 F16): RESOLVE blesses the IP, then a
     * numeric CONNECT to that IP presents an SNI for an unrelated vhost.
     * The bless is hostname-derived and 127.0.0.1 has no exact-IP grant
     * (127.0.0.0/8 is a covering CIDR, not a vouch for this address), so the
     * Q6 gate must apply and refuse the mismatched SNI. */
    int port;
    int lfd = sink_listen(&port);
    struct sink_result res = {0, 0};
    struct sink_arg sarg = { lfd, &res };
    pthread_t th;
    pthread_create(&th, NULL, sink_once, &sarg);

    char *hosts[] = {"localhost", "127.0.0.0/8"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 2) == 0);

    int fd = numeric_connect_after_resolve(&ctx, port);
    unsigned char hello[512];
    size_t hlen = build_client_hello("evil.example.com", hello, sizeof(hello));
    write(fd, hello, hlen);

    char buf[16];
    ssize_t rd = read(fd, buf, sizeof(buf));
    assert(rd == 0 || (rd < 0 && errno == ECONNRESET));
    close(fd);

    pthread_join(th, NULL);
    assert(res.accepted == 1);
    assert(res.got_bytes == 0);   /* refused before any relay */

    proxy_stop(&ctx);
    close(lfd);
    printf("  PASS: numeric CONNECT to hostname-blessed IP gated (vhost hop refused)\n");
}

static void test_sni_numeric_blessed_match(void) {
    /* Same numeric path, but the SNI matches the granted hostname rule —
     * the narrowed grant passes and the relay proceeds. */
    int port;
    int lfd = sink_listen(&port);
    struct sink_result res = {0, 0};
    struct sink_arg sarg = { lfd, &res };
    pthread_t th;
    pthread_create(&th, NULL, sink_once, &sarg);

    char *hosts[] = {"localhost", "127.0.0.0/8"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 2) == 0);

    int fd = numeric_connect_after_resolve(&ctx, port);
    unsigned char hello[512];
    size_t hlen = build_client_hello("localhost", hello, sizeof(hello));
    assert(write(fd, hello, hlen) == (ssize_t)hlen);

    pthread_join(th, NULL);
    assert(res.accepted == 1);
    assert(res.got_bytes == 1);   /* relayed through */

    close(fd);
    proxy_stop(&ctx);
    close(lfd);
    printf("  PASS: numeric CONNECT to hostname-blessed IP, matching SNI relayed\n");
}

static void test_sni_numeric_exact_grant_exempt(void) {
    /* An exact literal-IP grant vouches for the address itself (same
     * exact-vs-covering distinction as the metadata carve-out) — the gate
     * must NOT apply even though the IP is also hostname-blessed, so an
     * operator-granted IP is never over-blocked by an unrelated SNI. */
    int port;
    int lfd = sink_listen(&port);
    struct sink_result res = {0, 0};
    struct sink_arg sarg = { lfd, &res };
    pthread_t th;
    pthread_create(&th, NULL, sink_once, &sarg);

    char *hosts[] = {"localhost", "127.0.0.0/8", "127.0.0.1"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 3) == 0);

    int fd = numeric_connect_after_resolve(&ctx, port);
    unsigned char hello[512];
    size_t hlen = build_client_hello("evil.example.com", hello, sizeof(hello));
    assert(write(fd, hello, hlen) == (ssize_t)hlen);

    pthread_join(th, NULL);
    assert(res.accepted == 1);
    assert(res.got_bytes == 1);   /* exempt: relayed despite mismatched SNI */

    close(fd);
    proxy_stop(&ctx);
    close(lfd);
    printf("  PASS: exact-IP grant exempt from the numeric SNI gate\n");
}

int main(void) {
    alarm(30);
    setup_tmpdir();

    printf("test_proxy_sni:\n");
    test_sni_mismatch_blocked();
    test_sni_match_allowed();
    test_sni_absent_failopen();
    test_sni_numeric_blessed_gated();
    test_sni_numeric_blessed_match();
    test_sni_numeric_exact_grant_exempt();

    cleanup_tmpdir();
    printf("All proxy SNI tests passed.\n");
    return 0;
}
