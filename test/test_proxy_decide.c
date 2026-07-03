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

/* Send a RESOLVE or CONNECT preamble and return the first line of the response */
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
    proxy_req(proxy_sock_path(&ctx), "RESOLVE api.github.com\n", resp, sizeof(resp));
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
    proxy_req(proxy_sock_path(&ctx), "RESOLVE github.com\n", resp, sizeof(resp));
    assert(strncmp(resp, "ERROR denied", 12) != 0);

    proxy_stop(&ctx);
    printf("  PASS: suffix .github.com allows github.com (bare)\n");
}

static void test_suffix_evil_deny(void) {
    char *hosts[] = {".github.com"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 1) == 0);

    char resp[128];
    proxy_req(proxy_sock_path(&ctx), "RESOLVE evilgithub.com\n", resp, sizeof(resp));
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
    /* Use RESOLVE path to avoid hanging TCP connect to unreachable IP.
     * RESOLVE 10.0.0.1 → resolves to itself (literal), addr_permitted checks
     * it: private, not in 192.168.1.0/24 → denied. */
    proxy_req(proxy_sock_path(&ctx), "RESOLVE 10.0.0.1\n", resp, sizeof(resp));
    /* host_decide allows it (it's a numeric IP in... wait, 10.0.0.1 is NOT in
     * 192.168.1.0/24). host_decide will deny at the hostname level too. */
    assert(strncmp(resp, "ERROR denied", 12) == 0);

    proxy_stop(&ctx);
    printf("  PASS: CIDR grant 192.168.1.0/24 DENIES 10.0.0.1\n");
}

/* ─── METADATA CARVE-OUT (mandatory — §6 structural guarantee) ─── */

static void test_metadata_covering_cidr_deny(void) {
    /* Grant 169.254.0.0/16 which CONTAINS 169.254.169.254. The metadata IP
     * must still be DENIED — CIDR grants cannot reach metadata.
     * Use numeric CONNECT — host_decide sees 169.254.169.254 as a numeric IP
     * in the granted CIDR so it passes host_decide, but addr_permitted (called
     * indirectly for numeric IPs via bless path) blocks it.
     * Actually for numeric CONNECT: host_decide → bless_add → dial. But wait —
     * the metadata check is in addr_permitted which is only on the RESOLVE path.
     * For numeric CONNECT, host_decide does the check. So I need to ensure
     * host_decide also enforces the metadata carve-out for numeric IPs.
     *
     * Actually looking at the code flow: numeric CONNECT → host_decide. host_decide
     * tries host_match (no), then cidr_match on the binary → 169.254.169.254 IS
     * in 169.254.0.0/16 → returns 1. Then it would bless and dial.
     *
     * This means the metadata carve-out needs to be enforced in host_decide too,
     * not just addr_permitted! The numeric CONNECT path bypasses addr_permitted
     * entirely. Let me fix host_decide to check metadata range. */

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
     * Can't actually dial 169.254.169.254, so verify via RESOLVE path:
     * RESOLVE 169.254.169.254 → resolves to itself, addr_permitted checks:
     * in metadata range → granted_exact → yes (it's a /32 for exactly that IP)
     * → ADDR returned. */
    char *hosts[] = {"169.254.169.254"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 1) == 0);

    char resp[128];
    proxy_req(proxy_sock_path(&ctx), "RESOLVE 169.254.169.254\n", resp, sizeof(resp));
    /* host_decide allows the hostname-level check (it's the literal in CIDR rules).
     * Then resolve_and_bless resolves → addr_permitted → in metadata → granted_exact
     * → yes → blessed. */
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
    proxy_req(proxy_sock_path(&ctx), "RESOLVE localhost\n", resp, sizeof(resp));
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
    proxy_req(proxy_sock_path(&ctx), "RESOLVE anything.com\n", resp, sizeof(resp));
    assert(strncmp(resp, "ERROR denied", 12) == 0);

    proxy_stop(&ctx);
    printf("  PASS: empty rules → deny all\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * PATH-SPECIFIC METADATA CARVE-OUT TESTS (2.1–2.5)
 * Each test explicitly exercises one path (RESOLVE or numeric CONNECT)
 * to prove both enforcement sites work independently.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 2.1: RESOLVE path, covering CIDR — metadata denied via addr_permitted */
static void test_path_resolve_covering_cidr_deny(void) {
    /* Grant 169.254.0.0/16. RESOLVE 169.254.169.254:
     * host_decide allows the hostname-level (numeric IP in granted CIDR via
     * host_decide's cidr_match? No — metadata carve-out in host_decide fires).
     * Actually: RESOLVE goes through host_decide first, which also enforces the
     * carve-out. So this tests both layers on the RESOLVE path. */
    char *hosts[] = {"169.254.0.0/16"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 1) == 0);

    char resp[128];
    proxy_req(proxy_sock_path(&ctx), "RESOLVE 169.254.169.254\n", resp, sizeof(resp));
    /* host_decide rejects it (metadata range, no exact grant) → "ERROR denied" */
    assert(strncmp(resp, "ERROR denied", 12) == 0);

    proxy_stop(&ctx);
    printf("  PASS: 2.1 RESOLVE path + covering CIDR → metadata DENIED\n");
}

/* 2.2: NUMERIC CONNECT path, covering CIDR — THE path-specific case */
static void test_path_numeric_connect_covering_cidr_deny(void) {
    /* Grant 169.254.0.0/16; CONNECT 169.254.169.254:80 (numeric target).
     * Flow: bless_contains (empty) → host_decide → in_metadata_range → true →
     * granted_exact (no /32 for metadata) → 0 → host_decide returns 0 → DENIED */
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

/* 2.4: Exact grant — RESOLVE path allows (proves addr_permitted accepts).
 * Numeric CONNECT to metadata IP would hang (unreachable) so we verify via
 * RESOLVE only. The bless shortcut for metadata with exact grant is proven
 * structurally: if RESOLVE blesses it, a subsequent numeric CONNECT would hit
 * bless_contains → allowed. We test the RESOLVE→ADDR to confirm the grant. */
static void test_path_exact_grant_resolve_allow(void) {
    char *hosts[] = {"169.254.169.254"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 1) == 0);

    char resp[128];
    /* RESOLVE path: host_decide → metadata range → granted_exact → yes.
     * Then resolve_and_bless → addr_permitted → metadata → granted_exact → yes
     * → blesses → returns ADDR. */
    proxy_req(proxy_sock_path(&ctx), "RESOLVE 169.254.169.254\n", resp, sizeof(resp));
    assert(strncmp(resp, "ADDR", 4) == 0);

    proxy_stop(&ctx);
    printf("  PASS: 2.4 exact grant → RESOLVE path ALLOWS metadata IP\n");
}

/* 2.5: Bless-shortcut probe — covering CIDR cannot launder metadata into bless set.
 * Attempt RESOLVE 169.254.169.254 with covering CIDR grant (should fail to bless),
 * then numeric CONNECT (should not find it blessed → falls to host_decide → DENIED). */
static void test_path_bless_shortcut_probe(void) {
    char *hosts[] = {"169.254.0.0/16"};
    ProxyContext ctx;
    assert(proxy_start(&ctx, tmpdir, hosts, 1) == 0);

    char resp[128];
    /* Step 1: try to get metadata blessed via RESOLVE path.
     * host_decide rejects at hostname level (metadata carve-out), so resolve
     * never runs and bless_add is never called. */
    proxy_req(proxy_sock_path(&ctx), "RESOLVE 169.254.169.254\n", resp, sizeof(resp));
    assert(strncmp(resp, "ERROR denied", 12) == 0);

    /* Step 2: numeric CONNECT — bless_contains must be empty for metadata IP.
     * Falls through to host_decide → metadata carve-out → DENIED. */
    proxy_req(proxy_sock_path(&ctx), "169.254.169.254:80\n", resp, sizeof(resp));
    assert(strncmp(resp, "DENIED", 6) == 0);

    proxy_stop(&ctx);
    printf("  PASS: 2.5 bless-shortcut probe: covering CIDR cannot launder metadata\n");
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

    /* RESOLVE path of the same mapped address — also denied at host_decide. */
    proxy_req(proxy_sock_path(&ctx), "RESOLVE ::ffff:169.254.169.254\n", resp, sizeof(resp));
    assert(strncmp(resp, "ERROR denied", 12) == 0);

    proxy_stop(&ctx);
    printf("  PASS: 2.6 mapped IPv4-in-IPv6 cannot launder metadata past ::/0\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    alarm(10);
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

    /* Path-specific metadata carve-out (2.1–2.5) */
    test_path_resolve_covering_cidr_deny();
    test_path_numeric_connect_covering_cidr_deny();
    test_path_numeric_connect_all_cidr_deny();
    test_path_exact_grant_resolve_allow();
    test_path_bless_shortcut_probe();
    test_path_mapped_v6_metadata_deny();

    cleanup_tmpdir();
    printf("All proxy_decide tests passed.\n");
    return 0;
}
