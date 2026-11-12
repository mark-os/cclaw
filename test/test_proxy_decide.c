/* test_proxy_decide.c — integration tests for the decide() pipeline:
 * suffix matching, CIDR grants, and the metadata carve-out (§6).
 * Uses the UDS proxy directly; no real network beyond loopback. */
#define _POSIX_C_SOURCE 200809L
#include "proxy.h"
#include "test_proxy_util.h"
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "test_util.h"

static char tmpdir[128];

static void setup_tmpdir(void) {
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/test_proxy_decide_XXXXXX");
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

/* Ask the decision function directly — no socket, no dial. Formats the
 * verdict as one line so the assertions read like the wire protocol. */
static int probe(const ProxyContext *ctx, const char *target, char *resp, int cap) {
    char ip[64];
    int v = proxy_decide(ctx, target, ip, sizeof(ip), NULL);
    if (v == PROXY_ALLOWED) return snprintf(resp, (size_t)cap, "ADDR %s\n", ip);
    return snprintf(resp, (size_t)cap, "%s",
                    v == PROXY_DENIED ? "ERROR denied\n" : "ERROR resolve\n");
}

/* Send a CONNECT preamble and return the first line of the response */
static int proxy_req(const char *sock, const char *req, char *resp, int resp_cap) {
    int fd = uds_connect(sock);
    if (fd < 0) return -1;
    write(fd, req, strlen(req));
    int n = read_line(fd, resp, resp_cap);
    close(fd);
    return n;
}

/* ─── Suffix matching tests ─── */

static void test_suffix_subdomain_allow(void) {
    char *hosts[] = {".github.com"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 1) == 0);

    char resp[128];
    /* api.github.com is a subdomain — should be allowed at hostname level */
    probe(&ctx, "api.github.com", resp, sizeof(resp));
    /* It resolves to a public IP, so we get ADDR (or ERROR resolve if DNS fails
     * in CI — but the point is it's NOT "ERROR denied") */
    assert(strncmp(resp, "ERROR denied", 12) != 0);

    proxy_stop(&ctx);
    printf("  PASS: suffix .github.com allows api.github.com\n");
}

static void test_suffix_bare_allow(void) {
    char *hosts[] = {".github.com"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 1) == 0);

    char resp[128];
    probe(&ctx, "github.com", resp, sizeof(resp));
    assert(strncmp(resp, "ERROR denied", 12) != 0);

    proxy_stop(&ctx);
    printf("  PASS: suffix .github.com allows github.com (bare)\n");
}

static void test_suffix_evil_deny(void) {
    char *hosts[] = {".github.com"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 1) == 0);

    char resp[128];
    probe(&ctx, "evilgithub.com", resp, sizeof(resp));
    assert(strncmp(resp, "ERROR denied", 12) == 0);

    proxy_stop(&ctx);
    printf("  PASS: suffix .github.com DENIES evilgithub.com\n");
}

/* ─── CIDR grant tests ─── */

static void test_cidr_grant_private_allow(void) {
    /* Grant 127.0.0.0/8 — a numeric CONNECT to 127.0.0.1 should be allowed
     * (it's private but in the granted CIDR). Use a real loopback listener so
     * the dial succeeds without hanging. */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    assert(lfd >= 0);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    assert(bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    assert(listen(lfd, 1) == 0);
    socklen_t al = sizeof(sa);
    getsockname(lfd, (struct sockaddr *)&sa, &al);
    int port = ntohs(sa.sin_port);

    char *hosts[] = {"127.0.0.0/8"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 1) == 0);

    char req[64], resp[128];
    snprintf(req, sizeof(req), "127.0.0.1:%d\n", port);
    proxy_req(proxy_sock_path(&ctx), req, resp, sizeof(resp));
    /* Should get "OK" (dial succeeds to our listener), NOT "DENIED" */
    assert(strncmp(resp, "OK", 2) == 0);

    proxy_stop(&ctx);
    close(lfd);
    printf("  PASS: CIDR grant 127.0.0.0/8 allows 127.0.0.1\n");
}

static void test_cidr_grant_out_of_range_deny(void) {
    /* Grant only 192.168.1.0/24 — 10.0.0.1 is out of range and private → DENIED */
    char *hosts[] = {"192.168.1.0/24"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 1) == 0);

    char resp[128];
    /* Asked of the decision function, so nothing is dialed: a private
     * literal outside every granted CIDR. */
    probe(&ctx, "10.0.0.1", resp, sizeof(resp));
    assert(strncmp(resp, "ERROR denied", 12) == 0);

    proxy_stop(&ctx);
    printf("  PASS: CIDR grant 192.168.1.0/24 DENIES 10.0.0.1\n");
}

/* ─── METADATA CARVE-OUT (mandatory — §6 structural guarantee) ─── */

static void test_metadata_covering_cidr_deny(void) {
    /* Grant 169.254.0.0/16 which CONTAINS 169.254.169.254. The metadata IP
     * must still be DENIED — CIDR grants cannot reach metadata.
     * A numeric target is judged by host_decide alone, so the carve-out has
     * to live there (it does: numeric_ip_permitted checks the metadata range
     * before any CIDR match). */

    char *hosts[] = {"169.254.0.0/16"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 1) == 0);

    char resp[128];
    proxy_req(proxy_sock_path(&ctx), "169.254.169.254:80\n", resp, sizeof(resp));
    assert(strncmp(resp, "DENIED", 6) == 0);

    proxy_stop(&ctx);
    printf("  PASS: metadata carve-out: 169.254.0.0/16 grant DENIES 169.254.169.254\n");
}

static void test_metadata_wildcard_cidr_deny(void) {
    /* Grant 0.0.0.0/0 (everything) — metadata IP STILL denied. */
    char *hosts[] = {"0.0.0.0/0"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 1) == 0);

    char resp[128];
    proxy_req(proxy_sock_path(&ctx), "169.254.169.254:80\n", resp, sizeof(resp));
    assert(strncmp(resp, "DENIED", 6) == 0);

    proxy_stop(&ctx);
    printf("  PASS: metadata carve-out: 0.0.0.0/0 grant DENIES 169.254.169.254\n");
}

static void test_metadata_exact_grant_allow(void) {
    /* Grant the exact metadata IP literal — this IS allowed.
     * Asked of the decision function (169.254.169.254 cannot be dialed here):
     * in the metadata range → granted_exact → yes, and the literal is itself
     * the address to dial. */
    char *hosts[] = {"169.254.169.254"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 1) == 0);

    char resp[128];
    probe(&ctx, "169.254.169.254", resp, sizeof(resp));
    assert(strncmp(resp, "ADDR", 4) == 0);

    proxy_stop(&ctx);
    printf("  PASS: metadata carve-out: exact 169.254.169.254 grant ALLOWS\n");
}

/* ─── SSRF: hostname resolving to private IP, no CIDR grant → DENY ─── */

static void test_ssrf_private_resolve_deny(void) {
    /* "localhost" is granted as a hostname, but resolves to 127.0.0.1/::1.
     * Without a CIDR grant covering loopback, it must be DENIED. */
    char *hosts[] = {"localhost"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 1) == 0);

    char resp[128];
    probe(&ctx, "localhost", resp, sizeof(resp));
    /* resolve succeeds at DNS level but all results are private and ungrated → ERROR */
    assert(strncmp(resp, "ERROR", 5) == 0);

    proxy_stop(&ctx);
    printf("  PASS: SSRF: localhost resolves private, no CIDR grant → DENY\n");
}

/* ─── Default deny: empty rules ─── */

static void test_empty_rules_deny(void) {
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, NULL, 0) == 0);

    char resp[128];
    probe(&ctx, "anything.com", resp, sizeof(resp));
    assert(strncmp(resp, "ERROR denied", 12) == 0);

    proxy_stop(&ctx);
    printf("  PASS: empty rules → deny all\n");
}

/* ─── Sensitive-host DENY list (deny-before-allow) ─── */

static void test_deny_overrides_allow(void) {
    /* Host is in both allow and deny → denied (deny wins unconditionally). */
    char *hosts[] = {".github.com"};
    char *deny[]  = {"github.com"};
    ProxyContext ctx;
    assert(proxy_start_with_deny(&ctx, tmpdir, hosts, 1, deny, 1) == 0);

    char resp[128];
    probe(&ctx, "api.github.com", resp, sizeof(resp));
    assert(strncmp(resp, "ERROR denied", 12) == 0);

    proxy_stop(&ctx);
    printf("  PASS: deny overrides allow: github.com denied despite .github.com allow\n");
}

static void test_deny_subdomain_blocked(void) {
    /* Broad allow suffix covers the host, but deny label blocks subdomain. */
    char *hosts[] = {".example.com"};
    char *deny[]  = {"secret.example.com"};
    ProxyContext ctx;
    assert(proxy_start_with_deny(&ctx, tmpdir, hosts, 1, deny, 1) == 0);

    char resp[128];
    /* api.secret.example.com is a subdomain of the denied label */
    probe(&ctx, "api.secret.example.com", resp, sizeof(resp));
    assert(strncmp(resp, "ERROR denied", 12) == 0);

    /* The exact denied host too */
    probe(&ctx, "secret.example.com", resp, sizeof(resp));
    assert(strncmp(resp, "ERROR denied", 12) == 0);

    proxy_stop(&ctx);
    printf("  PASS: deny subdomain: secret.example.com blocks itself and sub-subdomains\n");
}

static void test_deny_unrelated_host_unaffected(void) {
    /* Deny list for one host does not interfere with other allowed targets.
     * A numeric IP grant keeps this DNS-free (no real network in unit tests —
     * a hostname that passes the allow check would hit the resolver). */
    char *hosts[] = {".github.com", "127.0.0.1"};
    char *deny[]  = {"github.com"};
    ProxyContext ctx;
    assert(proxy_start_with_deny(&ctx, tmpdir, hosts, 2, deny, 1) == 0);

    char resp[128];
    probe(&ctx, "127.0.0.1", resp, sizeof(resp));
    assert(strncmp(resp, "ERROR denied", 12) != 0);

    proxy_stop(&ctx);
    printf("  PASS: deny unrelated: 127.0.0.1 unaffected by github.com deny\n");
}

static void test_deny_empty_changes_nothing(void) {
    /* Empty deny list (NULL, 0) — allow logic unchanged. Numeric IP grant
     * keeps this DNS-free (no real network in unit tests). */
    char *hosts[] = {"127.0.0.1"};
    ProxyContext ctx;
    assert(proxy_start_with_deny(&ctx, tmpdir, hosts, 1, NULL, 0) == 0);

    char resp[128];
    probe(&ctx, "127.0.0.1", resp, sizeof(resp));
    assert(strncmp(resp, "ERROR denied", 12) != 0);

    proxy_stop(&ctx);
    printf("  PASS: empty deny list changes nothing\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * METADATA CARVE-OUT, BY ENTRY POINT (2.1–2.7)
 * The decision function asked directly, and a numeric CONNECT over the
 * socket: the carve-out must hold at both.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 2.1: decision function, covering CIDR — metadata denied */
static void test_path_resolve_covering_cidr_deny(void) {
    /* Grant 169.254.0.0/16 and ask about 169.254.169.254: in the metadata
     * range with no exact grant. */
    char *hosts[] = {"169.254.0.0/16"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 1) == 0);

    char resp[128];
    probe(&ctx, "169.254.169.254", resp, sizeof(resp));
    /* host_decide rejects it (metadata range, no exact grant) → "ERROR denied" */
    assert(strncmp(resp, "ERROR denied", 12) == 0);

    proxy_stop(&ctx);
    printf("  PASS: 2.1 decision + covering CIDR → metadata DENIED\n");
}

/* 2.2: NUMERIC CONNECT path, covering CIDR — THE path-specific case */
static void test_path_numeric_connect_covering_cidr_deny(void) {
    /* Grant 169.254.0.0/16; CONNECT 169.254.169.254:80 (numeric target).
     * Flow: host_decide → in_metadata_range → true → granted_exact (no /32
     * for metadata) → 0 → DENIED */
    char *hosts[] = {"169.254.0.0/16"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 1) == 0);

    char resp[128];
    proxy_req(proxy_sock_path(&ctx), "169.254.169.254:80\n", resp, sizeof(resp));
    assert(strncmp(resp, "DENIED", 6) == 0);

    proxy_stop(&ctx);
    printf("  PASS: 2.2 numeric CONNECT + covering CIDR → metadata DENIED\n");
}

/* 2.3: NUMERIC CONNECT path, 0.0.0.0/0 grant */
static void test_path_numeric_connect_all_cidr_deny(void) {
    char *hosts[] = {"0.0.0.0/0"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 1) == 0);

    char resp[128];
    proxy_req(proxy_sock_path(&ctx), "169.254.169.254:80\n", resp, sizeof(resp));
    assert(strncmp(resp, "DENIED", 6) == 0);

    proxy_stop(&ctx);
    printf("  PASS: 2.3 numeric CONNECT + 0.0.0.0/0 → metadata DENIED\n");
}

/* 2.4: Exact grant — allowed. A numeric CONNECT to the metadata IP would
 * hang (unreachable), so this asks the decision function only. */
static void test_path_exact_grant_resolve_allow(void) {
    char *hosts[] = {"169.254.169.254"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 1) == 0);

    char resp[128];
    /* host_decide → metadata range → granted_exact → yes. */
    probe(&ctx, "169.254.169.254", resp, sizeof(resp));
    assert(strncmp(resp, "ADDR", 4) == 0);

    proxy_stop(&ctx);
    printf("  PASS: 2.4 exact grant ALLOWS metadata IP\n");
}

/* 2.5: Asking first changes nothing — the proxy keeps no memory between
 * requests (the per-address "bless" set went with the RESOLVE request), so a
 * denied question cannot prime a later numeric CONNECT. */
static void test_path_no_state_between_requests(void) {
    char *hosts[] = {"169.254.0.0/16"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 1) == 0);

    char resp[128];
    probe(&ctx, "169.254.169.254", resp, sizeof(resp));
    assert(strncmp(resp, "ERROR denied", 12) == 0);

    /* Then a numeric CONNECT: host_decide → metadata carve-out → DENIED. */
    proxy_req(proxy_sock_path(&ctx), "169.254.169.254:80\n", resp, sizeof(resp));
    assert(strncmp(resp, "DENIED", 6) == 0);

    proxy_stop(&ctx);
    printf("  PASS: 2.5 no state between requests: covering CIDR cannot launder metadata\n");
}

/* 2.6: IPv4-mapped IPv6 must not launder a metadata IP past the carve-out.
 * Grant ::/0 (covers every v6 address incl. ::ffff:169.254.169.254). A numeric
 * CONNECT to the mapped form of the metadata IP must still be DENIED: ip_to_bin
 * canonicalizes ::ffff:a.b.c.d to its v4 form, so host_decide sees 169.254.169.254,
 * hits in_metadata_range, and requires an exact grant (which ::/0 is not). */
static void test_path_mapped_v6_metadata_deny(void) {
    char *hosts[] = {"::/0"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 1) == 0);

    char resp[128];
    proxy_req(proxy_sock_path(&ctx), "::ffff:169.254.169.254:80\n", resp, sizeof(resp));
    assert(strncmp(resp, "DENIED", 6) == 0);

    /* The decision function on the same mapped address — also denied. */
    probe(&ctx, "::ffff:169.254.169.254", resp, sizeof(resp));
    assert(strncmp(resp, "ERROR denied", 12) == 0);

    proxy_stop(&ctx);
    printf("  PASS: 2.6 mapped IPv4-in-IPv6 cannot launder metadata past ::/0\n");
}

/* 2.7: "*" host grant allows an ordinary hostname (the wildcard's
 * actual purpose) but still cannot launder the metadata IP — the numeric
 * CIDR pipeline in host_decide runs before host_match ever sees "*". */
static void test_path_wildcard_allows_host_denies_metadata(void) {
    char *hosts[] = {"*"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 1) == 0);

    char resp[128];
    probe(&ctx, "example.com", resp, sizeof(resp));
    assert(strncmp(resp, "ADDR", 4) == 0);

    proxy_req(proxy_sock_path(&ctx), "169.254.169.254:80\n", resp, sizeof(resp));
    assert(strncmp(resp, "DENIED", 6) == 0);

    probe(&ctx, "169.254.169.254", resp, sizeof(resp));
    assert(strncmp(resp, "ERROR denied", 12) == 0);

    proxy_stop(&ctx);
    printf("  PASS: 2.7 \"*\" grant ALLOWS a hostname, still DENIES metadata\n");
}

int main(void) {
    TEST_INIT();
    /* Each proxy start/stop cycle costs ~0.5s (accept-loop wake latency), and
     * the suite runs ~20 of them — keep the alarm under the Makefile's 20s
     * timeout wrapper but with headroom for the whole set. */
    alarm(18);
    setup_tmpdir();
    printf("test_proxy_decide:\n");

    /* Suffix matching */
    test_suffix_subdomain_allow();
    test_suffix_bare_allow();
    test_suffix_evil_deny();

    /* CIDR grants */
    test_cidr_grant_private_allow();
    test_cidr_grant_out_of_range_deny();

    /* Metadata carve-out — MANDATORY */
    test_metadata_covering_cidr_deny();
    test_metadata_wildcard_cidr_deny();
    test_metadata_exact_grant_allow();

    /* SSRF */
    test_ssrf_private_resolve_deny();

    /* Default deny */
    test_empty_rules_deny();

    /* Sensitive-host deny list */
    test_deny_overrides_allow();
    test_deny_subdomain_blocked();
    test_deny_unrelated_host_unaffected();
    test_deny_empty_changes_nothing();

    /* Path-specific metadata carve-out (2.1–2.5) */
    test_path_resolve_covering_cidr_deny();
    test_path_numeric_connect_covering_cidr_deny();
    test_path_numeric_connect_all_cidr_deny();
    test_path_exact_grant_resolve_allow();
    test_path_no_state_between_requests();
    test_path_mapped_v6_metadata_deny();
    test_path_wildcard_allows_host_denies_metadata();

    cleanup_tmpdir();
    printf("All proxy_decide tests passed.\n");
    return 0;
}
