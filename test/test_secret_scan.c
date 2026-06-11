#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "secret_scan.h"

#define PASS(name) printf("  PASS: %s\n", name)

static void test_aws_key(void) {
    const char *text = "here is AKIAIOSFODNN7EXAMPLE ok";
    ScanFinding f[8];
    int n = secret_scan(text, strlen(text), f, 8);
    assert(n >= 1);
    assert(strcmp(f[0].rule_id, "aws-access-token") == 0);
    assert(f[0].offset == 8);
    PASS("aws-access-token detected");
}

static void test_github_pat(void) {
    /* ghp_ + exactly 36 alnum chars */
    const char *text = "found ghp_aB3kL9mXp7qR2wNv4jH8sT5uY1cF6gW0eDzQ here";
    ScanFinding f[8];
    int n = secret_scan(text, strlen(text), f, 8);
    assert(n >= 1);
    int found = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(f[i].rule_id, "github-pat") == 0) found = 1;
    assert(found);
    PASS("github-pat detected");
}

static void test_private_key(void) {
    const char *text = "-----BEGIN RSA PRIVATE KEY-----\nMIIE...\n-----END RSA PRIVATE KEY-----";
    ScanFinding f[8];
    int n = secret_scan(text, strlen(text), f, 8);
    assert(n >= 1);
    assert(strcmp(f[0].rule_id, "private-key") == 0);
    PASS("private-key detected");
}

static void test_anthropic_key(void) {
    /* sk-ant-api03- followed by 93 alnum chars */
    char buf[256] = "key: sk-ant-api03-";
    int plen = (int)strlen(buf);
    for (int i = 0; i < 93; i++) buf[plen + i] = 'A' + (i % 26);
    buf[plen + 93] = '\0';
    ScanFinding f[16];
    int n = secret_scan(buf, strlen(buf), f, 16);
    assert(n >= 1);
    int found = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(f[i].rule_id, "anthropic-api-key") == 0) found = 1;
    assert(found);
    PASS("anthropic-api-key detected");
}

static void test_generic_api_key(void) {
    /* High-entropy value after "api_key=" */
    const char *text = "config: api_key=aB3kL9mXp7qR2wNv4jH8 done";
    ScanFinding f[8];
    int n = secret_scan(text, strlen(text), f, 8);
    assert(n >= 1);
    assert(strcmp(f[0].rule_id, "generic-api-key") == 0);
    PASS("generic-api-key high-entropy detected");
}

static void test_reject_low_entropy(void) {
    /* Low-entropy value after "token=" should NOT match */
    const char *text = "token=testvalue123";
    ScanFinding f[8];
    int n = secret_scan(text, strlen(text), f, 8);
    /* "testvalue123" has low entropy, should be rejected */
    int found_generic = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(f[i].rule_id, "generic-api-key") == 0) found_generic = 1;
    assert(!found_generic);
    PASS("reject low-entropy generic");
}

static void test_reject_short_akia(void) {
    /* AKIA followed by only 10 chars (need 16) */
    const char *text = "AKIA1234567890 too short";
    ScanFinding f[8];
    int n = secret_scan(text, strlen(text), f, 8);
    int found_aws = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(f[i].rule_id, "aws-access-token") == 0) found_aws = 1;
    assert(!found_aws);
    PASS("reject short AKIA");
}

static void test_redact(void) {
    char buf[512];
    snprintf(buf, sizeof(buf), "secret: AKIAIOSFODNN7EXAMPLE end");
    size_t len = strlen(buf);
    int n = secret_scan_redact(buf, &len, sizeof(buf));
    assert(n >= 1);
    assert(strstr(buf, "[SECRET_DETECTED:") != NULL);
    assert(strstr(buf, "AKIAIOSFODNN7EXAMPLE") == NULL);
    PASS("redact replaces aws key");
}

static void test_entropy_calculation(void) {
    /* All same char = 0 entropy */
    float e1 = secret_scan_entropy("aaaaaaaaaa", 10);
    assert(e1 < 0.01f);
    /* High entropy random-looking string */
    float e2 = secret_scan_entropy("aB3kL9mXp7qR2wNv", 17);
    assert(e2 > 3.0f);
    PASS("entropy calculation");
}

int main(void) {
    printf("test_secret_scan:\n");
    test_aws_key();
    test_github_pat();
    test_private_key();
    test_anthropic_key();
    test_generic_api_key();
    test_reject_low_entropy();
    test_reject_short_akia();
    test_redact();
    test_entropy_calculation();
    printf("  ALL PASSED\n");
    return 0;
}
