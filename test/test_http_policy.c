#include <stdio.h>
#include "http_policy.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  " name "... "); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

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

int main(void) {
    printf("test_http_policy:\n");
    test_is_private_ip();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
