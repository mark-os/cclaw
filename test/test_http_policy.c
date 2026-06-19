#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "http_policy.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  " name "... "); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

/* NULL policy = unrestricted */
static void test_null_policy_allows_all(void) {
    TEST("null_policy_allows_all");
    char err[128] = {0};
    int rc = http_check_policy("https://anything.com/path", NULL, err, sizeof(err));
    if (rc != 0) { FAIL(err); return; }
    PASS();
}

/* Invalid scheme rejected */
static void test_invalid_scheme(void) {
    TEST("invalid_scheme");
    HttpPolicy p = {.block_private = 1};
    char err[128] = {0};
    int rc = http_check_policy("ftp://example.com", &p, err, sizeof(err));
    if (rc == 0) { FAIL("should reject ftp"); return; }
    if (!strstr(err, "http://")) { FAIL(err); return; }
    PASS();
}

/* Blocked host rejected */
static void test_blocked_host(void) {
    TEST("blocked_host");
    char *blocked[] = {"evil.com"};
    HttpPolicy p = {.blocked_hosts = blocked, .blocked_count = 1, .block_private = 0};
    char err[128] = {0};
    int rc = http_check_policy("https://evil.com/data", &p, err, sizeof(err));
    if (rc == 0) { FAIL("should reject blocked host"); return; }
    if (!strstr(err, "blocked")) { FAIL(err); return; }
    PASS();
}

/* Allowed host passes */
static void test_allowed_host_passes(void) {
    TEST("allowed_host_passes");
    char *allowed[] = {"api.example.com"};
    HttpPolicy p = {.allowed_hosts = allowed, .allowed_count = 1, .block_private = 0};
    char err[128] = {0};
    int rc = http_check_policy("https://api.example.com/v1", &p, err, sizeof(err));
    if (rc != 0) { FAIL(err); return; }
    PASS();
}

/* Host not in allowed list rejected */
static void test_host_not_allowed(void) {
    TEST("host_not_allowed");
    char *allowed[] = {"api.example.com"};
    HttpPolicy p = {.allowed_hosts = allowed, .allowed_count = 1, .block_private = 0};
    char err[128] = {0};
    int rc = http_check_policy("https://other.com/data", &p, err, sizeof(err));
    if (rc == 0) { FAIL("should reject unlisted host"); return; }
    if (!strstr(err, "not in allowed_hosts")) { FAIL(err); return; }
    PASS();
}

/* Empty allowed list = allow all (only blocked + SSRF apply) */
static void test_empty_allowed_allows_all(void) {
    TEST("empty_allowed_allows_all");
    HttpPolicy p = {.allowed_hosts = NULL, .allowed_count = 0, .block_private = 0};
    char err[128] = {0};
    int rc = http_check_policy("https://anything.com/", &p, err, sizeof(err));
    if (rc != 0) { FAIL(err); return; }
    PASS();
}

/* SSRF: private IP rejected when block_private=1 */
static void test_ssrf_localhost(void) {
    TEST("ssrf_localhost");
    char *allowed[] = {"localhost"};
    HttpPolicy p = {.allowed_hosts = allowed, .allowed_count = 1, .block_private = 1};
    char err[128] = {0};
    int rc = http_check_policy("http://localhost/secret", &p, err, sizeof(err));
    if (rc == 0) { FAIL("should reject localhost"); return; }
    if (!strstr(err, "private IP")) { FAIL(err); return; }
    PASS();
}

/* SSRF: 127.0.0.1 rejected */
static void test_ssrf_127(void) {
    TEST("ssrf_127.0.0.1");
    char *allowed[] = {"127.0.0.1"};
    HttpPolicy p = {.allowed_hosts = allowed, .allowed_count = 1, .block_private = 1};
    char err[128] = {0};
    int rc = http_check_policy("http://127.0.0.1/secret", &p, err, sizeof(err));
    if (rc == 0) { FAIL("should reject 127.0.0.1"); return; }
    if (!strstr(err, "private IP")) { FAIL(err); return; }
    PASS();
}

/* SSRF disabled: private IP allowed when block_private=0 */
static void test_ssrf_disabled(void) {
    TEST("ssrf_disabled_allows_private");
    char *allowed[] = {"127.0.0.1"};
    HttpPolicy p = {.allowed_hosts = allowed, .allowed_count = 1, .block_private = 0};
    char err[128] = {0};
    int rc = http_check_policy("http://127.0.0.1/test", &p, err, sizeof(err));
    if (rc != 0) { FAIL(err); return; }
    PASS();
}

/* is_private_ip unit tests */
static void test_is_private_ip(void) {
    TEST("is_private_ip");
    /* private / unsafe — expect 1 */
    if (!http_is_private_ip("127.0.0.1")) { FAIL("127.0.0.1"); return; }
    if (!http_is_private_ip("10.0.0.1")) { FAIL("10.0.0.1"); return; }
    if (!http_is_private_ip("172.16.0.1")) { FAIL("172.16.0.1"); return; }
    if (!http_is_private_ip("172.31.255.255")) { FAIL("172.31.255.255"); return; }
    if (!http_is_private_ip("192.168.1.1")) { FAIL("192.168.1.1"); return; }
    if (!http_is_private_ip("169.254.1.1")) { FAIL("169.254.1.1"); return; }
    if (!http_is_private_ip("100.64.0.1")) { FAIL("100.64.0.1 CGNAT"); return; }
    if (!http_is_private_ip("100.127.255.255")) { FAIL("100.127.255.255 CGNAT"); return; }
    if (!http_is_private_ip("0.0.0.0")) { FAIL("0.0.0.0"); return; }
    if (!http_is_private_ip("0.1.2.3")) { FAIL("0.1.2.3"); return; }
    if (!http_is_private_ip("::1")) { FAIL("::1"); return; }
    if (!http_is_private_ip("fe80::1")) { FAIL("fe80::1"); return; }
    if (!http_is_private_ip("fc00::1")) { FAIL("fc00::1"); return; }
    if (!http_is_private_ip("fd12::34")) { FAIL("fd12::34"); return; }
    if (!http_is_private_ip("::ffff:127.0.0.1")) { FAIL("::ffff:127.0.0.1"); return; }
    if (!http_is_private_ip("::ffff:10.0.0.1")) { FAIL("::ffff:10.0.0.1"); return; }
    /* public / safe — expect 0 */
    if (http_is_private_ip("8.8.8.8")) { FAIL("8.8.8.8 should be public"); return; }
    if (http_is_private_ip("1.1.1.1")) { FAIL("1.1.1.1 should be public"); return; }
    if (http_is_private_ip("100.63.255.255")) { FAIL("100.63.255.255 should be public"); return; }
    if (http_is_private_ip("100.128.0.0")) { FAIL("100.128.0.0 should be public"); return; }
    if (http_is_private_ip("172.15.0.1")) { FAIL("172.15.0.1 should be public"); return; }
    if (http_is_private_ip("172.32.0.1")) { FAIL("172.32.0.1 should be public"); return; }
    if (http_is_private_ip("2606:4700:4700::1111")) { FAIL("2606:4700:4700::1111 should be public"); return; }
    if (http_is_private_ip("::ffff:8.8.8.8")) { FAIL("::ffff:8.8.8.8 should be public"); return; }
    PASS();
}

/* extract_host unit tests */
static void test_extract_host(void) {
    TEST("extract_host");
    char host[256];
    if (http_extract_host("https://example.com/path", host, sizeof(host)) != 0) { FAIL("basic"); return; }
    if (strcmp(host, "example.com") != 0) { FAIL(host); return; }
    if (http_extract_host("http://user@host.io:8080/x", host, sizeof(host)) != 0) { FAIL("userinfo"); return; }
    if (strcmp(host, "host.io") != 0) { FAIL(host); return; }
    if (http_extract_host("https://sub.domain.org?q=1", host, sizeof(host)) != 0) { FAIL("query"); return; }
    if (strcmp(host, "sub.domain.org") != 0) { FAIL(host); return; }
    PASS();
}

/* Blocked takes priority over allowed */
static void test_blocked_overrides_allowed(void) {
    TEST("blocked_overrides_allowed");
    char *allowed[] = {"evil.com"};
    char *blocked[] = {"evil.com"};
    HttpPolicy p = {
        .allowed_hosts = allowed, .allowed_count = 1,
        .blocked_hosts = blocked, .blocked_count = 1,
        .block_private = 0
    };
    char err[128] = {0};
    int rc = http_check_policy("https://evil.com/", &p, err, sizeof(err));
    if (rc == 0) { FAIL("blocked should override allowed"); return; }
    if (!strstr(err, "blocked")) { FAIL(err); return; }
    PASS();
}

int main(void) {
    printf("test_http_policy:\n");
    test_null_policy_allows_all();
    test_invalid_scheme();
    test_blocked_host();
    test_allowed_host_passes();
    test_host_not_allowed();
    test_empty_allowed_allows_all();
    test_ssrf_localhost();
    test_ssrf_127();
    test_ssrf_disabled();
    test_is_private_ip();
    test_extract_host();
    test_blocked_overrides_allowed();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
