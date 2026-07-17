/* test_secret_scan_coverage.c — ensure every rule in scan_rules[] fires.
 * Generates a synthetic token for each rule and asserts detection.
 * Relies on .github/secret_scanning.yml excluding test/ from push protection. */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "secret_scan.h"
#include "secret_scan_rules.h"
#include "test_util.h"

#define PASS(name) printf("  PASS: %s\n", name)

/* Generate a high-entropy tail of the correct charset into buf[0..len-1].
 * Uses a deterministic pattern that exceeds any entropy threshold ≤ 4.5.
 * Each pattern uses all chars in its alphabet to maximize entropy. */
static void gen_tail(char *buf, int len, int charset) {
    /* 62 unique chars → entropy ~5.9 bits/byte at length ≥ 62 */
    static const char alnum[] =
        "aB3kL9mXp7qR2wNv4jH8sT5uY1cF6gW0eDzIbOfAiQlJhMrZtKxSyCnUqEP";
    /* 16 unique chars → entropy 4.0 bits/byte at length ≥ 16 */
    static const char hex[] = "0a1b2c3d4e5f6789abcdef0123456789abcdef";
    /* 36 unique chars → entropy ~5.2 bits/byte */
    static const char upper[] = "A3B7C2D9E4F8G1H5J6K0L3M7N2P9Q4R8S1T5U";
    /* 38 unique chars → entropy ~5.2 bits/byte */
    static const char lower[] = "a3b7c2d9e_f8g1h-5j6k0l3m7n2p9q4r8s1t5u";
    const char *src;
    int slen;
    switch (charset) {
    case 4: /* CHARSET_HEX */
        src = hex; slen = (int)sizeof(hex) - 1; break;
    case 1: /* CHARSET_UPPER_ALNUM */
        src = upper; slen = (int)sizeof(upper) - 1; break;
    case 2: /* CHARSET_LOWER_ALNUM */
        src = lower; slen = (int)sizeof(lower) - 1; break;
    default: /* CHARSET_ALNUM / ANY */
        src = alnum; slen = (int)sizeof(alnum) - 1; break;
    }
    for (int i = 0; i < len; i++)
        buf[i] = src[i % slen];
}

/* Test a PREFIX rule: prefix + tail of correct charset/length */
static int test_prefix(int rule_idx) {
    const ScanRule *r = &scan_rules[rule_idx];
    char buf[2048];
    int klen = (int)strlen(r->keyword);
    /* Use tail_min, but if entropy threshold is high and tail_min is short,
     * use a longer tail to ensure enough unique chars for entropy. */
    int tail_len = r->tail_min;
    if (r->entropy >= 3.5f && tail_len < 20) tail_len = 20;
    if (r->entropy >= 4.0f && tail_len < 32) tail_len = 32;
    if (tail_len > r->tail_max) tail_len = r->tail_max;

    /* Build: "found " + keyword + tail + " end" */
    int off = 0;
    memcpy(buf + off, "found ", 6); off += 6;
    memcpy(buf + off, r->keyword, (size_t)klen); off += klen;
    gen_tail(buf + off, tail_len, r->charset);
    off += tail_len;
    memcpy(buf + off, " end", 5); off += 5;

    ScanFinding f[SCAN_MAX_FINDINGS];
    int n = secret_scan(buf, (size_t)(off - 1), f, SCAN_MAX_FINDINGS);
    for (int i = 0; i < n; i++)
        if (strcmp(f[i].rule_id, r->id) == 0) return 1;
    return 0;
}

/* Test a KEYWORD rule: "keyword = <high-entropy-value>" */
static int test_keyword(int rule_idx) {
    const ScanRule *r = &scan_rules[rule_idx];
    char buf[512];
    int klen = (int)strlen(r->keyword);
    int val_len = r->tail_min > 16 ? r->tail_min : 16;

    int off = 0;
    memcpy(buf + off, r->keyword, (size_t)klen); off += klen;
    memcpy(buf + off, " = \"", 4); off += 4;
    gen_tail(buf + off, val_len, r->charset);
    off += val_len;
    memcpy(buf + off, "\"\n", 3); off += 3;

    ScanFinding f[SCAN_MAX_FINDINGS];
    int n = secret_scan(buf, (size_t)(off - 1), f, SCAN_MAX_FINDINGS);
    for (int i = 0; i < n; i++)
        if (strcmp(f[i].rule_id, r->id) == 0) return 1;
    return 0;
}

/* Test a BASE64 rule: anchor + base64 of a user:pass pair */
static int test_base64(int rule_idx) {
    const ScanRule *r = &scan_rules[rule_idx];
    /* base64("admin:password") — decodes to printable ASCII with a colon. */
    static const char sample[] = "YWRtaW46cGFzc3dvcmQ=";
    char buf[256];
    int off = snprintf(buf, sizeof(buf), "found %s%s end", r->keyword, sample);

    ScanFinding f[SCAN_MAX_FINDINGS];
    int n = secret_scan(buf, (size_t)off, f, SCAN_MAX_FINDINGS);
    for (int i = 0; i < n; i++)
        if (strcmp(f[i].rule_id, r->id) == 0) return 1;
    return 0;
}

/* Test a LITERAL rule: just the keyword present with word boundary */
static int test_literal(int rule_idx) {
    const ScanRule *r = &scan_rules[rule_idx];
    char buf[256];
    int off = snprintf(buf, sizeof(buf), "file: %s RSA PRIVATE KEY-----", r->keyword);

    ScanFinding f[SCAN_MAX_FINDINGS];
    int n = secret_scan(buf, (size_t)off, f, SCAN_MAX_FINDINGS);
    for (int i = 0; i < n; i++)
        if (strcmp(f[i].rule_id, r->id) == 0) return 1;
    return 0;
}

/* Test a SPAN rule: full block from anchor through the END marker */
static int test_span(int rule_idx) {
    const ScanRule *r = &scan_rules[rule_idx];
    char buf[512];
    int off = snprintf(buf, sizeof(buf),
                       "file: %s RSA PRIVATE KEY-----\nMIIEfakebody\n"
                       "-----END RSA PRIVATE KEY-----\n", r->keyword);

    ScanFinding f[SCAN_MAX_FINDINGS];
    int n = secret_scan(buf, (size_t)off, f, SCAN_MAX_FINDINGS);
    for (int i = 0; i < n; i++)
        if (strcmp(f[i].rule_id, r->id) == 0) return 1;
    return 0;
}

int main(void) {
    TEST_INIT();
    printf("test_secret_scan_coverage:\n");

    int passed = 0, failed = 0, skipped = 0;

    for (int i = 0; i < SCAN_RULE_COUNT; i++) {
        const ScanRule *r = &scan_rules[i];

        /* Skip rules that are mathematically unfirable: entropy threshold
         * exceeds log2(tail_max) — can't have 4.0 bits/byte in 11 chars. */
        if (r->vtype == SCAN_VTYPE_PREFIX && r->entropy > 0.0f &&
            r->tail_max <= 15 && r->entropy >= 4.0f) {
            printf("  SKIP: rule %d %s (%s) — entropy %.1f impossible in %d chars\n",
                   i, r->id, r->keyword, r->entropy, r->tail_max);
            skipped++;
            continue;
        }
        int ok = 0;

        switch (r->vtype) {
        case SCAN_VTYPE_PREFIX:  ok = test_prefix(i); break;
        case SCAN_VTYPE_KEYWORD: ok = test_keyword(i); break;
        case SCAN_VTYPE_LITERAL: ok = test_literal(i); break;
        case SCAN_VTYPE_BASE64:  ok = test_base64(i); break;
        case SCAN_VTYPE_SPAN:    ok = test_span(i); break;
        }

        if (ok) {
            printf("  PASS: rule %d %s (%s)\n", i, r->id, r->keyword);
            passed++;
        } else {
            printf("  FAIL: rule %d %s (%s)\n", i, r->id, r->keyword);
            failed++;
        }
    }

    printf("\n  %d/%d rules covered", passed, SCAN_RULE_COUNT);
    if (skipped) printf(", %d skipped (unfirable)", skipped);
    if (failed) printf(", %d FAILED", failed);
    printf("\n");
    assert(failed == 0);
    printf("  ALL PASSED\n");
    return 0;
}
