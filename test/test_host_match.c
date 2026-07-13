#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include "host_match.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  " name "... "); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

/* ─── host_match ─── */

static void test_exact_match(void) {
    TEST("exact_match");
    char *rules[] = {"github.com", "api.example.com"};
    if (!host_match(rules, 2, "github.com")) FAIL("should match");
    if (!host_match(rules, 2, "api.example.com")) FAIL("should match api");
    if (host_match(rules, 2, "evil.com")) FAIL("should not match");
    PASS();
}

static void test_suffix_match_subdomain(void) {
    TEST("suffix_match_subdomain");
    char *rules[] = {".github.com"};
    if (!host_match(rules, 1, "api.github.com")) FAIL("subdomain should match");
    if (!host_match(rules, 1, "deep.api.github.com")) FAIL("deep sub should match");
    PASS();
}

static void test_suffix_match_bare(void) {
    TEST("suffix_match_bare_domain");
    char *rules[] = {".github.com"};
    /* ".github.com" should match "github.com" (the base) */
    if (!host_match(rules, 1, "github.com")) FAIL("bare base should match");
    PASS();
}

static void test_suffix_no_false_positive(void) {
    TEST("suffix_no_evilgithub");
    char *rules[] = {".github.com"};
    /* MUST NOT match "evilgithub.com" — the dot boundary is load-bearing */
    if (host_match(rules, 1, "evilgithub.com")) FAIL("evilgithub.com must NOT match");
    if (host_match(rules, 1, "notgithub.com")) FAIL("notgithub.com must NOT match");
    PASS();
}

static void test_case_insensitive_ascii(void) {
    TEST("case_insensitive_ascii");
    /* ASCII fold must hold regardless of process locale (review-3 F2) */
    char *rules[] = {"GitHub.com", ".Example.COM"};
    if (!host_match(rules, 2, "github.com")) FAIL("exact should fold case");
    if (!host_match(rules, 2, "API.EXAMPLE.com")) FAIL("suffix should fold case");
    PASS();
}

static void test_empty_rules_deny_all(void) {
    TEST("empty_rules_deny_all");
    if (host_match(NULL, 0, "anything.com")) FAIL("NULL rules should deny");
    char *rules[] = {};
    if (host_match(rules, 0, "x.com")) FAIL("empty array should deny");
    PASS();
}

static void test_wildcard_matches_any_host(void) {
    TEST("wildcard_matches_any_host");
    char *rules[] = {"*"};
    if (!host_match(rules, 1, "anything.example")) FAIL("* should match arbitrary host");
    if (!host_match(rules, 1, "a")) FAIL("* should match single-label host");
    PASS();
}

static void test_wildcard_is_bare_only(void) {
    TEST("wildcard_is_bare_only");
    /* "*.example.com" is NOT the wildcard sentinel — it's an unimplemented
     * per-label glob (TODO Q3), so it must be treated as a literal (and thus
     * useless) exact-match rule, never silently promoted to allow-all. */
    char *rules[] = {"*.example.com"};
    if (host_match(rules, 1, "api.example.com")) FAIL("*.example.com must not glob-match");
    if (host_match(rules, 1, "example.com")) FAIL("*.example.com must not match the base either");
    PASS();
}

/* ─── host_in_text ─── */

static void test_hit_exact_and_subdomain(void) {
    TEST("hit_exact_and_subdomain");
    char *labels[] = {"chase.com"};
    char got[254];
    if (!host_in_text(labels, 1, "{\"url\":\"https://chase.com/login\"}", got, sizeof(got)))
        FAIL("exact label in url should hit");
    if (strcmp(got, "chase.com") != 0) FAIL("wrong token for exact");
    if (!host_in_text(labels, 1, "curl -s api.chase.com/transfer", got, sizeof(got)))
        FAIL("subdomain should hit (registered-domain semantics)");
    if (strcmp(got, "api.chase.com") != 0) FAIL("wrong token for subdomain");
    PASS();
}

static void test_lookalikes_do_not_hit(void) {
    TEST("lookalikes_do_not_hit");
    char *labels[] = {"chase.com"};
    if (host_in_text(labels, 1, "visit mychase.com now", NULL, 0))
        FAIL("prefix lookalike must NOT hit");
    if (host_in_text(labels, 1, "https://chase.com.evil.co/x", NULL, 0))
        FAIL("suffix lookalike must NOT hit");
    if (host_in_text(labels, 1, "chasexcom or chase-com", NULL, 0))
        FAIL("non-host text must NOT hit");
    PASS();
}

static void test_text_punctuation_boundaries(void) {
    TEST("text_punctuation_boundaries");
    char *labels[] = {"bank.example"};
    char got[64];
    /* Token embedded in JSON quotes, trailing dot prose, case */
    if (!host_in_text(labels, 1, "go to BANK.EXAMPLE.", got, sizeof(got)))
        FAIL("case-insensitive + trailing dot should hit");
    if (strcmp(got, "BANK.EXAMPLE") != 0) FAIL("token should be trimmed of edge dot");
    if (host_in_text(labels, 1, "", NULL, 0)) FAIL("empty text");
    if (host_in_text(NULL, 0, "bank.example", NULL, 0)) FAIL("no labels = no hit");
    PASS();
}

static void test_suffix_label_form(void) {
    TEST("suffix_label_form");
    char *labels[] = {".pay.example.com"};
    if (!host_in_text(labels, 1, "POST https://api.pay.example.com/v1", NULL, 0))
        FAIL("suffix-form label should cover subdomain");
    if (!host_in_text(labels, 1, "pay.example.com", NULL, 0))
        FAIL("suffix-form label should cover base");
    if (host_in_text(labels, 1, "prepay.example.com", NULL, 0))
        FAIL("dot boundary must hold for suffix-form label");
    PASS();
}

/* ─── cidr_parse ─── */

static void test_parse_v4_cidr(void) {
    TEST("parse_v4_cidr");
    Cidr c;
    if (cidr_parse("192.168.1.0/24", &c) != 0) FAIL("parse failed");
    if (c.family != AF_INET) FAIL("wrong family");
    if (c.prefix_len != 24) FAIL("wrong prefix_len");
    /* Host bits zeroed: 192.168.1.0 */
    unsigned char expect[] = {192, 168, 1, 0};
    if (memcmp(c.prefix, expect, 4) != 0) FAIL("wrong prefix bits");
    PASS();
}

static void test_parse_v4_bare_ip(void) {
    TEST("parse_v4_bare_ip");
    Cidr c;
    if (cidr_parse("10.0.0.5", &c) != 0) FAIL("parse failed");
    if (c.prefix_len != 32) FAIL("bare IP should be /32");
    unsigned char expect[] = {10, 0, 0, 5};
    if (memcmp(c.prefix, expect, 4) != 0) FAIL("wrong addr");
    PASS();
}

static void test_parse_v4_host_bits_zeroed(void) {
    TEST("parse_v4_host_bits_zeroed");
    Cidr c;
    /* 192.168.1.100/24 → prefix should be 192.168.1.0 */
    if (cidr_parse("192.168.1.100/24", &c) != 0) FAIL("parse failed");
    unsigned char expect[] = {192, 168, 1, 0};
    if (memcmp(c.prefix, expect, 4) != 0) FAIL("host bits not zeroed");
    PASS();
}

static void test_parse_v6_cidr(void) {
    TEST("parse_v6_cidr");
    Cidr c;
    if (cidr_parse("fe80::/10", &c) != 0) FAIL("parse failed");
    if (c.family != AF_INET6) FAIL("wrong family");
    if (c.prefix_len != 10) FAIL("wrong prefix_len");
    PASS();
}

static void test_parse_v6_bare(void) {
    TEST("parse_v6_bare_loopback");
    Cidr c;
    if (cidr_parse("::1", &c) != 0) FAIL("parse failed");
    if (c.prefix_len != 128) FAIL("bare v6 should be /128");
    unsigned char lo[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
    if (memcmp(c.prefix, lo, 16) != 0) FAIL("wrong addr");
    PASS();
}

static void test_parse_rejects_malformed_prefix(void) {
    TEST("parse_rejects_malformed_prefix");
    Cidr c;
    /* Empty or non-numeric prefix length must NOT default to /0 (allow-all). */
    if (cidr_parse("10.0.0.0/", &c) == 0) FAIL("trailing-slash must reject");
    if (cidr_parse("1.2.3.4/xyz", &c) == 0) FAIL("non-numeric prefix must reject");
    if (cidr_parse("1.2.3.4/33", &c) == 0) FAIL("v4 prefix > 32 must reject");
    if (cidr_parse("fe80::/129", &c) == 0) FAIL("v6 prefix > 128 must reject");
    PASS();
}

/* ─── cidr_match ─── */

static void test_cidr_match_v4_in_range(void) {
    TEST("cidr_match_v4_in_range");
    Cidr rules[1];
    cidr_parse("192.168.1.0/24", &rules[0]);
    struct in_addr a;
    inet_pton(AF_INET, "192.168.1.50", &a);
    if (!cidr_match(rules, 1, AF_INET, (unsigned char *)&a)) FAIL("should match");
    PASS();
}

static void test_cidr_match_v4_out_of_range(void) {
    TEST("cidr_match_v4_out_of_range");
    Cidr rules[1];
    cidr_parse("192.168.1.0/24", &rules[0]);
    struct in_addr a;
    inet_pton(AF_INET, "192.168.2.50", &a);
    if (cidr_match(rules, 1, AF_INET, (unsigned char *)&a)) FAIL("should NOT match");
    PASS();
}

static void test_cidr_match_v4_32(void) {
    TEST("cidr_match_v4_/32_exact");
    Cidr rules[1];
    cidr_parse("10.0.0.5", &rules[0]);  /* bare → /32 */
    struct in_addr a;
    inet_pton(AF_INET, "10.0.0.5", &a);
    if (!cidr_match(rules, 1, AF_INET, (unsigned char *)&a)) FAIL("exact should match");
    inet_pton(AF_INET, "10.0.0.6", &a);
    if (cidr_match(rules, 1, AF_INET, (unsigned char *)&a)) FAIL("/32 should not match neighbor");
    PASS();
}

static void test_cidr_match_v6(void) {
    TEST("cidr_match_v6_fe80");
    Cidr rules[1];
    cidr_parse("fe80::/10", &rules[0]);
    struct in6_addr a6;
    inet_pton(AF_INET6, "fe80::1", &a6);
    if (!cidr_match(rules, 1, AF_INET6, (unsigned char *)&a6)) FAIL("link-local should match");
    inet_pton(AF_INET6, "2001:db8::1", &a6);
    if (cidr_match(rules, 1, AF_INET6, (unsigned char *)&a6)) FAIL("global should NOT match");
    PASS();
}

static void test_cidr_match_v6_128(void) {
    TEST("cidr_match_v6_/128_exact");
    Cidr rules[1];
    cidr_parse("::1", &rules[0]);
    struct in6_addr a6;
    inet_pton(AF_INET6, "::1", &a6);
    if (!cidr_match(rules, 1, AF_INET6, (unsigned char *)&a6)) FAIL("loopback should match");
    inet_pton(AF_INET6, "::2", &a6);
    if (cidr_match(rules, 1, AF_INET6, (unsigned char *)&a6)) FAIL("::2 should not match /128");
    PASS();
}

/* ─── granted_exact ─── */

static void test_granted_exact_hit(void) {
    TEST("granted_exact_hit");
    Cidr rules[2];
    cidr_parse("169.254.0.0/16", &rules[0]);   /* covering CIDR — NOT exact */
    cidr_parse("169.254.169.254", &rules[1]);   /* exact /32 */
    struct in_addr a;
    inet_pton(AF_INET, "169.254.169.254", &a);
    if (!granted_exact(rules, 2, AF_INET, (unsigned char *)&a)) FAIL("exact grant should match");
    PASS();
}

static void test_granted_exact_cidr_not_exact(void) {
    TEST("granted_exact_cidr_does_not_count");
    Cidr rules[1];
    cidr_parse("169.254.0.0/16", &rules[0]);   /* covers it but not exact */
    struct in_addr a;
    inet_pton(AF_INET, "169.254.169.254", &a);
    if (granted_exact(rules, 1, AF_INET, (unsigned char *)&a))
        FAIL("CIDR must NOT count as exact — metadata carve-out broken");
    PASS();
}

static void test_granted_exact_0_0_0_0_not_exact(void) {
    TEST("granted_exact_0.0.0.0/0_not_exact");
    Cidr rules[1];
    cidr_parse("0.0.0.0/0", &rules[0]);
    struct in_addr a;
    inet_pton(AF_INET, "169.254.169.254", &a);
    if (granted_exact(rules, 1, AF_INET, (unsigned char *)&a))
        FAIL("0.0.0.0/0 must NOT pass exact check");
    PASS();
}

/* ─── metadata carve-out integration (addr_permitted logic) ─── */

static void test_metadata_carveout_covering_cidr_deny(void) {
    TEST("metadata_carveout: covering CIDR denies metadata IP");
    /* Simulate: granted 169.254.0.0/16 (covers metadata), but no exact grant.
     * The metadata IP must be DENIED via granted_exact, not via cidr_match. */
    Cidr rules[1];
    cidr_parse("169.254.0.0/16", &rules[0]);
    struct in_addr a;
    inet_pton(AF_INET, "169.254.169.254", &a);
    /* cidr_match WOULD say yes — that's the containment check */
    if (!cidr_match(rules, 1, AF_INET, (unsigned char *)&a))
        FAIL("sanity: cidr_match should contain it");
    /* But granted_exact says NO — metadata path must use this */
    if (granted_exact(rules, 1, AF_INET, (unsigned char *)&a))
        FAIL("metadata carve-out violated");
    PASS();
}

static void test_metadata_carveout_exact_grant_allow(void) {
    TEST("metadata_carveout: exact grant allows metadata IP");
    Cidr rules[1];
    cidr_parse("169.254.169.254", &rules[0]);
    struct in_addr a;
    inet_pton(AF_INET, "169.254.169.254", &a);
    if (!granted_exact(rules, 1, AF_INET, (unsigned char *)&a))
        FAIL("exact metadata grant should allow");
    PASS();
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_host_match\n");

    /* host_match */
    test_exact_match();
    test_suffix_match_subdomain();
    test_suffix_match_bare();
    test_suffix_no_false_positive();
    test_case_insensitive_ascii();
    test_empty_rules_deny_all();
    test_wildcard_matches_any_host();
    test_wildcard_is_bare_only();

    /* host_in_text */
    test_hit_exact_and_subdomain();
    test_lookalikes_do_not_hit();
    test_text_punctuation_boundaries();
    test_suffix_label_form();

    /* cidr_parse */
    test_parse_v4_cidr();
    test_parse_v4_bare_ip();
    test_parse_v4_host_bits_zeroed();
    test_parse_v6_cidr();
    test_parse_v6_bare();
    test_parse_rejects_malformed_prefix();

    /* cidr_match */
    test_cidr_match_v4_in_range();
    test_cidr_match_v4_out_of_range();
    test_cidr_match_v4_32();
    test_cidr_match_v6();
    test_cidr_match_v6_128();

    /* granted_exact */
    test_granted_exact_hit();
    test_granted_exact_cidr_not_exact();
    test_granted_exact_0_0_0_0_not_exact();

    /* metadata carve-out logic */
    test_metadata_carveout_covering_cidr_deny();
    test_metadata_carveout_exact_grant_allow();

    printf("%d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
