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
    const char *text = "cat key.pem:\n-----BEGIN RSA PRIVATE KEY-----\n"
        "MIIEow7fake8key8body8line\nQhs8second8line\n"
        "-----END RSA PRIVATE KEY-----\ndone";
    ScanFinding f[8];
    int n = secret_scan(text, strlen(text), f, 8);
    assert(n >= 1);
    assert(strcmp(f[0].rule_id, "private-key") == 0);
    /* Span must cover the key BODY through the END marker, not just the
     * BEGIN header (which left the key material in cleartext). */
    const char *end = strstr(text, "KEY-----\ndone");
    assert(f[0].offset == 13);
    assert(f[0].offset + f[0].match_len == (int)(end - text) + 8);
    PASS("private-key spans body through END marker");
}

static void test_private_key_truncated(void) {
    /* END marker cut off: everything from BEGIN onward is redacted. */
    const char *text = "-----BEGIN OPENSSH PRIVATE KEY-----\nb3BlbnNzaC1rZXk";
    ScanFinding f[8];
    int n = secret_scan(text, strlen(text), f, 8);
    assert(n >= 1);
    assert(strcmp(f[0].rule_id, "private-key") == 0);
    assert(f[0].offset == 0);
    assert(f[0].match_len == (int)strlen(text));
    PASS("private-key truncated block redacts to end");
}

static void test_certificate_not_private_key(void) {
    const char *text = "-----BEGIN CERTIFICATE-----\nMIIC...\n-----END CERTIFICATE-----";
    ScanFinding f[8];
    int n = secret_scan(text, strlen(text), f, 8);
    for (int i = 0; i < n; i++)
        assert(strcmp(f[i].rule_id, "private-key") != 0);
    PASS("certificate header does not match private-key");
}

static void test_fine_grained_pat_full(void) {
    /* github_pat_ + 82 word chars: the whole token must be covered — the old
     * LITERAL rule matched only the prefix and leaked the 82-char tail. */
    char buf[160] = "t: github_pat_";
    size_t plen = strlen(buf);
    for (int i = 0; i < 82; i++) buf[plen + (size_t)i] = "aB3kL9mX_p7"[i % 11];
    buf[plen + 82] = '\0';
    ScanFinding f[8];
    int n = secret_scan(buf, strlen(buf), f, 8);
    int found = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(f[i].rule_id, "github-fine-grained-pat") == 0) {
            found = 1;
            assert(f[i].offset == 3);
            assert(f[i].match_len == 11 + 82);
        }
    assert(found);
    PASS("fine-grained PAT covers full token");
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

static void test_redact_zero_slack(void) {
    /* Regression: with cap = len+1 (every inbox/CLI call site) and a tag
     * longer than the match, scan_replace used to silently leave the secret
     * in place while redact reported success. */
    char buf[128];
    snprintf(buf, sizeof(buf), "my key AKIAIOSFODNN7EXAMPLE is for the s3 bucket");
    size_t len = strlen(buf);
    int n = secret_scan_redact(buf, &len, len + 1);
    assert(n == -1);  /* bytes dropped */
    assert(strstr(buf, "AKIAIOSFODNN7EXAMPLE") == NULL);
    assert(strstr(buf, "[SECRET_DETECTED:") != NULL);
    assert(strstr(buf, " is for") != NULL);  /* tail kept up to capacity */
    PASS("zero-slack redact removes secret, keeps fitting tail");
}

static void test_redact_zero_slack_multi(void) {
    /* Regression: a truncated replacement must not abort the back-to-front
     * loop — the findings left of the truncation still need redacting. */
    char buf[160];
    snprintf(buf, sizeof(buf),
             "a AKIAIOSFODNN7EXAMPLE b AKIAIOSFODNN7EXAMPL2 c");
    size_t len = strlen(buf);
    int n = secret_scan_redact(buf, &len, len + 1);
    assert(n == -1);
    assert(strstr(buf, "AKIAIOSFODNN7EXAMPL") == NULL);
    PASS("zero-slack redact handles multiple findings");
}

static void test_no_false_positive_words(void) {
    /* Regression: the jwt rule used to degenerate to anchor "ey" and flag
     * every "ey" bigram; collapsing hex/lower charsets to generic ALNUM made
     * the "sk"/"s." anchors match file paths, base64 and URLs. */
    const char *texts[] = {
        "they said the monkey had a key",
        "/home/user/Desktop/node_modules/react/index.js",
        "https://docs.google.com/spreadsheets/d/abc123def456",
        "please review the survey results before friday",
    };
    for (size_t t = 0; t < sizeof(texts) / sizeof(texts[0]); t++) {
        ScanFinding f[8];
        int n = secret_scan(texts[t], strlen(texts[t]), f, 8);
        for (int i = 0; i < n; i++)
            printf("    unexpected: %s in \"%s\"\n", f[i].rule_id, texts[t]);
        assert(n == 0);
    }
    PASS("no false positives on ordinary text");
}

static void test_twilio_case_pinned(void) {
    /* twilio is SK[0-9a-fA-F]{32}: real uppercase key matches; the lowercase
     * "sk"+hex form and ordinary "sk" text do not (case-pinned, hex-only). */
    const char *hit  = "key SK0123456789abcdef0123456789abcdef rest";
    const char *miss_lower = "key sk0123456789abcdef0123456789abcdef rest";
    const char *miss_word  = "the Desktop folder and a task list";
    ScanFinding f[8];

    int n = secret_scan(hit, strlen(hit), f, 8);
    int found = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(f[i].rule_id, "twilio-api-key") == 0) found = 1;
    assert(found);

    n = secret_scan(miss_lower, strlen(miss_lower), f, 8);
    for (int i = 0; i < n; i++)
        assert(strcmp(f[i].rule_id, "twilio-api-key") != 0);

    n = secret_scan(miss_word, strlen(miss_word), f, 8);
    assert(n == 0);
    PASS("twilio case-pinned + hex tail");
}

static void test_boundary_required(void) {
    /* A prefix anchor glued to a preceding word char is not a real token
     * start (mirrors gitleaks' leading \b). */
    const char *text = "xghp_aB3kL9mXp7qR2wNv4jH8sT5uY1cF6gW0eDzQ";
    ScanFinding f[8];
    int n = secret_scan(text, strlen(text), f, 8);
    int found = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(f[i].rule_id, "github-pat") == 0) found = 1;
    assert(!found);
    PASS("reject prefix glued to preceding token");
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

static void test_saturation_signal(void) {
    /* 33 AWS keys — exceeds SCAN_MAX_FINDINGS (32) */
    char buf[2048] = {0};
    int off = 0;
    for (int i = 0; i < 33; i++) {
        off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                        "AKIA%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c ",
                        'A'+(i%26), 'B', 'C', 'D', 'E', 'F', 'G', 'H',
                        'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P');
    }
    ScanFinding f[SCAN_MAX_FINDINGS];
    int n = secret_scan(buf, strlen(buf), f, SCAN_MAX_FINDINGS);
    assert(n == SCAN_MAX_FINDINGS + 1);
    PASS("saturation signal (33 keys → max+1)");
}

static void test_saturation_exact(void) {
    /* Exactly 32 keys — should return 32, not saturated */
    char buf[2048] = {0};
    int off = 0;
    for (int i = 0; i < 32; i++) {
        off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                        "AKIA%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c ",
                        'A'+(i%26), 'B', 'C', 'D', 'E', 'F', 'G', 'H',
                        'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P');
    }
    ScanFinding f[SCAN_MAX_FINDINGS];
    int n = secret_scan(buf, strlen(buf), f, SCAN_MAX_FINDINGS);
    assert(n == 32);
    PASS("exact 32 keys → not saturated");
}

static void test_saturation_full_redact(void) {
    /* Saturation triggers full content replacement */
    char buf[2048] = {0};
    int off = 0;
    for (int i = 0; i < 33; i++) {
        off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                        "AKIA%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c ",
                        'A'+(i%26), 'B', 'C', 'D', 'E', 'F', 'G', 'H',
                        'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P');
    }
    size_t len = strlen(buf);
    int n = secret_scan_redact(buf, &len, sizeof(buf));
    assert(n == 1);
    assert(strcmp(buf, "[SECRET_DETECTED:content_redacted]") == 0);
    assert(strstr(buf, "AKIA") == NULL);
    PASS("saturation full redact");
}

static void test_keyword_boundary_and_charset(void) {
    /* Regression (pogoplug 2026-07-11): keyword rules fired all over source
     * code because they lacked the word-boundary check and accepted any
     * non-whitespace run as a "value". */
    const char *code[] = {
        /* "api" glued inside an identifier + directory-listing colon */
        "espn_api.qjs: 1234 bytes, modified yesterday snapshot",
        /* code expression as value: parens are outside the charset */
        "config.token = channel.getConfig(\"telegram\").token2",
        "  return JSON.parse(r.body).access; // token: JSON.parse(r.body)",
        /* SQL from cclaw's own source */
        "UPDATE sessions SET total_tokens_in=total_tokens_in+?1 WHERE id=?2",
    };
    for (size_t t = 0; t < sizeof(code) / sizeof(code[0]); t++) {
        ScanFinding f[8];
        int n = secret_scan(code[t], strlen(code[t]), f, 8);
        for (int i = 0; i < n; i++)
            printf("    unexpected: %s in \"%s\"\n", f[i].rule_id, code[t]);
        assert(n == 0);
    }
    /* Still catches a real assignment. */
    const char *real = "export API_TOKEN=aB3kL9mXp7qR2wNv4jH8";
    ScanFinding f[8];
    int n = secret_scan(real, strlen(real), f, 8);
    int found = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(f[i].rule_id, "generic-api-key") == 0) found = 1;
    assert(found);
    PASS("keyword boundary + charset reject code, keep real keys");
}

static void test_placeholder_skip(void) {
    /* The scanner must never fire inside an already-masked {{SECRET:...}}
     * placeholder — that's how nested {{SECRET:{{SECRET:... was minted. */
    const char *text = "result: {{SECRET:GITHUB_API_KEY_9}} used for the call";
    ScanFinding f[8];
    int n = secret_scan(text, strlen(text), f, 8);
    for (int i = 0; i < n; i++)
        printf("    unexpected: %s at %d\n", f[i].rule_id, f[i].offset);
    assert(n == 0);
    /* A secret NEXT to a placeholder is still caught. */
    const char *mixed = "{{SECRET:A_B2}} and AKIAIOSFODNN7EXAMPLE";
    n = secret_scan(mixed, strlen(mixed), f, 8);
    assert(n == 1);
    assert(strcmp(f[0].rule_id, "aws-access-token") == 0);
    PASS("placeholder regions skipped");
}

static void test_keyword_newline_stop(void) {
    /* Operator on next line — must NOT match */
    const char *cross = "api_key\n=aB3kL9mXp7qR2wNv4jH8sT5uY1cF6";
    ScanFinding f[8];
    int n = secret_scan(cross, strlen(cross), f, 8);
    int found = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(f[i].rule_id, "generic-api-key") == 0) found = 1;
    assert(!found);

    /* Same line — must match */
    const char *same = "api_key=aB3kL9mXp7qR2wNv4jH8sT5uY1cF6";
    n = secret_scan(same, strlen(same), f, 8);
    found = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(f[i].rule_id, "generic-api-key") == 0) found = 1;
    assert(found);
    PASS("keyword newline stop");
}

static int has_rule(const char *text, const char *rule_id) {
    ScanFinding f[32];
    int n = secret_scan(text, strlen(text), f, 32);
    if (n > 32) return 1; /* saturated — treat as present */
    for (int i = 0; i < n; i++)
        if (strcmp(f[i].rule_id, rule_id) == 0) return 1;
    return 0;
}

static void test_authorization_bearer(void) {
    /* Real credential after the scheme word must be caught. The generic rule
     * can't — it stops at "Bearer" (all-alpha) and never sees the token. */
    assert(has_rule("Authorization: Bearer "
        "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwfQ.dozjgNryP4J3jVmNHl0w5N",
        "http-authorization-bearer"));
    assert(has_rule("authorization:Bearer ghp_aB3kL9mXp7qR2wNv4jH8sT5uY1cF6dE0xZ",
        "http-authorization-bearer"));
    /* Prose, placeholders, and env refs must NOT fire. */
    assert(!has_rule("Use the Authorization: Bearer <token> header.",
        "http-authorization-bearer"));
    assert(!has_rule("Authorization: Bearer YOUR_TOKEN_HERE",
        "http-authorization-bearer"));
    assert(!has_rule("Send Authorization: Bearer ${ACCESS_TOKEN} now",
        "http-authorization-bearer"));
    assert(!has_rule("Bearer bonds are a fixed-income security.",
        "http-authorization-bearer"));
    PASS("authorization bearer");
}

static void test_authorization_basic(void) {
    /* Long credential — base64(username:supersecretpassword123). */
    assert(has_rule("Authorization: Basic "
        "dXNlcm5hbWU6c3VwZXJzZWNyZXRwYXNzd29yZDEyMw==",
        "http-authorization-basic"));
    /* Weak/short creds an entropy gate would miss: base64("admin:admin") and
     * base64("root:toor") decode to printable user:pass, so they're caught. */
    assert(has_rule("Authorization: Basic YWRtaW46YWRtaW4=",
        "http-authorization-basic"));
    assert(has_rule("Authorization: Basic cm9vdDp0b29y",
        "http-authorization-basic"));
    /* Prose must not fire — including "instructions", which is valid base64
     * *shape* (12 chars) but decodes to non-printable bytes with no colon. */
    assert(!has_rule("Basic authentication is required here.",
        "http-authorization-basic"));
    assert(!has_rule("Authorization: Basic instructions",
        "http-authorization-basic"));
    assert(!has_rule("Authorization: Basic configuration",
        "http-authorization-basic"));
    PASS("authorization basic (base64 decode)");
}

static void test_jwt_token(void) {
    /* Standalone JWT, no assignment context. */
    assert(has_rule("token is "
        "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOjEsImV4cCI6OTk5OX0."
        "abcDEF123456789xyzABCdef", "jwt-token"));
    /* Short "eyj..." words must not trip it. */
    assert(!has_rule("the key eyjafjallajokull is a volcano", "jwt-token"));
    assert(!has_rule("function f() { return eyjConfig; }", "jwt-token"));
    PASS("jwt token");
}

static void test_slack_variants(void) {
    assert(has_rule("xoxa-2-1111111111-2222222222-aBcDeFgHiJkLmNoPqRsTuVwX",
        "slack-misc-token"));
    assert(has_rule("xapp-1-A01B2C3D4E5-1234567890-abcdef1234567890abcdef",
        "slack-app-token"));
    PASS("slack token variants");
}

int main(void) {
    printf("test_secret_scan:\n");
    test_aws_key();
    test_github_pat();
    test_private_key();
    test_private_key_truncated();
    test_certificate_not_private_key();
    test_fine_grained_pat_full();
    test_keyword_boundary_and_charset();
    test_placeholder_skip();
    test_anthropic_key();
    test_generic_api_key();
    test_reject_low_entropy();
    test_reject_short_akia();
    test_no_false_positive_words();
    test_twilio_case_pinned();
    test_boundary_required();
    test_redact();
    test_redact_zero_slack();
    test_redact_zero_slack_multi();
    test_entropy_calculation();
    test_saturation_signal();
    test_saturation_exact();
    test_saturation_full_redact();
    test_keyword_newline_stop();
    test_authorization_bearer();
    test_authorization_basic();
    test_jwt_token();
    test_slack_variants();
    printf("  ALL PASSED\n");
    return 0;
}
