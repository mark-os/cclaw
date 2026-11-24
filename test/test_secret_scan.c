#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "secret_scan.h"
#include "test_util.h"

static int has_rule(const char *text, const char *rule_id) {
    ScanFinding f[32];
    int n = secret_scan(text, strlen(text), f, 32);
    if (n > 32) return 1; /* saturated — treat as present */
    for (int i = 0; i < n; i++)
        if (strcmp(f[i].rule_id, rule_id) == 0) return 1;
    return 0;
}

#define AZ "ABCDEFGHIJKLMNOPQRSTUVWXYZ"

/* One rule fires (or must not fire) on one input. */
static const struct { const char *text; const char *rule; int present; } detect[] = {
    {"found ghp_aB3kL9mXp7qR2wNv4jH8sT5uY1cF6gW0eDzQ here", "github-pat", 1},
    /* prefix glued to a preceding word char is not a token start */
    {"xghp_aB3kL9mXp7qR2wNv4jH8sT5uY1cF6gW0eDzQ", "github-pat", 0},
    {"-----BEGIN CERTIFICATE-----\nMIIC...\n-----END CERTIFICATE-----", "private-key", 0},
    {"key: sk-ant-api03-" AZ AZ AZ "ABCDEFGHIJKLMNO", "anthropic-api-key", 1},
    {"config: api_key=aB3kL9mXp7qR2wNv4jH8 done", "generic-api-key", 1},
    {"token=testvalue123", "generic-api-key", 0},              /* low entropy */
    {"AKIA1234567890 too short", "aws-access-token", 0},
    /* twilio is case-pinned SK + 32 hex */
    {"key SK0123456789abcdef0123456789abcdef rest", "twilio-api-key", 1},
    {"key sk0123456789abcdef0123456789abcdef rest", "twilio-api-key", 0},
    {"export API_TOKEN=aB3kL9mXp7qR2wNv4jH8", "generic-api-key", 1},
    /* keyword rules stop at a newline */
    {"api_key\n=aB3kL9mXp7qR2wNv4jH8sT5uY1cF6", "generic-api-key", 0},
    {"api_key=aB3kL9mXp7qR2wNv4jH8sT5uY1cF6", "generic-api-key", 1},
    /* Authorization: Bearer — the generic rule stops at "Bearer" */
    {"Authorization: Bearer "
     "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwfQ.dozjgNryP4J3jVmNHl0w5N",
     "http-authorization-bearer", 1},
    {"authorization:Bearer ghp_aB3kL9mXp7qR2wNv4jH8sT5uY1cF6dE0xZ", "http-authorization-bearer", 1},
    {"Use the Authorization: Bearer <token> header.", "http-authorization-bearer", 0},
    {"Authorization: Bearer YOUR_TOKEN_HERE", "http-authorization-bearer", 0},
    {"Send Authorization: Bearer ${ACCESS_TOKEN} now", "http-authorization-bearer", 0},
    {"Bearer bonds are a fixed-income security.", "http-authorization-bearer", 0},
    /* Basic: decoded printable user:pass, even weak/short creds */
    {"Authorization: Basic dXNlcm5hbWU6c3VwZXJzZWNyZXRwYXNzd29yZDEyMw==", "http-authorization-basic", 1},
    {"Authorization: Basic YWRtaW46YWRtaW4=", "http-authorization-basic", 1},
    {"Authorization: Basic cm9vdDp0b29y", "http-authorization-basic", 1},
    {"Basic authentication is required here.", "http-authorization-basic", 0},
    /* base64-shaped words that decode to non-printable bytes, no colon */
    {"Authorization: Basic instructions", "http-authorization-basic", 0},
    {"Authorization: Basic configuration", "http-authorization-basic", 0},
    {"token is eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOjEsImV4cCI6OTk5OX0."
     "abcDEF123456789xyzABCdef", "jwt-token", 1},
    {"the key eyjafjallajokull is a volcano", "jwt-token", 0},
    {"function f() { return eyjConfig; }", "jwt-token", 0},
    {"xoxa-2-1111111111-2222222222-aBcDeFgHiJkLmNoPqRsTuVwX", "slack-misc-token", 1},
    {"xapp-1-A01B2C3D4E5-1234567890-abcdef1234567890abcdef", "slack-app-token", 1},
};

static void test_detect_table(void) {
    for (size_t i = 0; i < sizeof(detect) / sizeof(detect[0]); i++) {
        if (has_rule(detect[i].text, detect[i].rule) != detect[i].present) {
            printf("    row %zu: %s expected %d on \"%s\"\n", i,
                   detect[i].rule, detect[i].present, detect[i].text);
            assert(0);
        }
    }
}

/* Inputs that must yield no finding at all and survive redaction
 * byte-identical. */
static const char *clean[] = {
    /* jwt rule used to degenerate to anchor "ey"; collapsed charsets made
     * "sk"/"s." match paths, base64 and URLs */
    "they said the monkey had a key",
    "/home/user/Desktop/node_modules/react/index.js",
    "https://docs.google.com/spreadsheets/d/abc123def456",
    "please review the survey results before friday",
    "the Desktop folder and a task list",
    /* pogoplug 2026-07-11: keyword rules lacked the word boundary and took
     * any non-space run as a value */
    "espn_api.qjs: 1234 bytes, modified yesterday snapshot",
    "config.token = channel.getConfig(\"telegram\").token2",
    "  return JSON.parse(r.body).access; // token: JSON.parse(r.body)",
    "UPDATE sessions SET total_tokens_in=total_tokens_in+?1 WHERE id=?2",
    /* an already-masked placeholder must not mint {{SECRET:{{SECRET:... */
    "result: {{SECRET:GITHUB_API_KEY_9}} used for the call",
    /* keyword words as prefixes of innocent words ("Secretary") */
    "The Secretary of State signed the memo this morning.",
    "Secretary: Jane Doe, minutes approved by the committee.",
    "the gland resumed secretion after the procedure",
    "Notes from the Secretariat: budget approved unanimously.",
    "He typed his passwordless login on the keyboard.",
    "apiary tokenizer accessibility authority monkey keyed",
    "secret: the meeting notes are in the shared folder",
    "password: please choose something memorable",
    "Access to the archive: ask the librarian on duty.",
    /* timestamps, epoch ids, SHAs, checksums */
    "2026-07-31T20:35:28.649Z INFO worker 3 started in 128ms",
    "[2026-07-31T20:35:28.649Z] request id=1753994128649 duration=128ms",
    "{\"created_at\":\"2026-07-31T20:35:28.649Z\","
    "\"id\":1753994128649,\"status\":\"ok\"}",
    "commit 9f2b4c1e8a7d6f503b21c4e9a8d7f605b3e1c2d4\n"
    "Author: Mark\nDate: 2026-07-31T20:35:28.649Z",
    "git log --oneline: 3f91a21 f701138 c976d5c 3d36c39 2896b2a",
    "sha256 checksums:\n"
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
    " file.tar.gz",
    "build 20260731203528 finished; 1753994128649 rows scanned",
};

static void test_clean_table(void) {
    for (size_t t = 0; t < sizeof(clean) / sizeof(clean[0]); t++) {
        ScanFinding f[SCAN_MAX_FINDINGS];
        size_t len = strlen(clean[t]);
        int n = secret_scan(clean[t], len, f, SCAN_MAX_FINDINGS);
        if (n != 0) {
            printf("    row %zu: %d findings (first %s) in \"%s\"\n", t, n,
                   n <= SCAN_MAX_FINDINGS ? f[0].rule_id : "saturated", clean[t]);
            assert(0);
        }
        char buf[512];
        assert(len + 1 <= sizeof(buf));
        memcpy(buf, clean[t], len + 1);
        assert(secret_scan_redact(buf, &len, sizeof(buf)) == 0);
        assert(strcmp(buf, clean[t]) == 0);
    }
}

/* Redaction removes only the credential; the named context survives. */
static const struct { const char *text; const char *secret; const char *kept; } leaks[] = {
    {"secret: AKIAIOSFODNN7EXAMPLE end", "AKIAIOSFODNN7EXAMPLE", "secret: "},
    {"The Secretary emailed api_key=aB3kL9mXp7qR2wNv4jH8 by mistake",
     "aB3kL9mXp7qR2wNv4jH8", "The Secretary emailed"},
    {"[2026-07-31T20:35:28.649Z] leaked AKIAIOSFODNN7EXAMPLE id=1753994128649",
     "AKIAIOSFODNN7EXAMPLE", "2026-07-31T20:35:28.649Z"},
    {"[2026-07-31T20:35:28.649Z] leaked AKIAIOSFODNN7EXAMPLE id=1753994128649",
     "AKIAIOSFODNN7EXAMPLE", "id=1753994128649"},
};

static void test_redact_table(void) {
    for (size_t i = 0; i < sizeof(leaks) / sizeof(leaks[0]); i++) {
        char buf[256];
        size_t len = strlen(leaks[i].text);
        memcpy(buf, leaks[i].text, len + 1);
        int n = secret_scan_redact(buf, &len, sizeof(buf));
        if (n < 1 || strstr(buf, leaks[i].secret) || !strstr(buf, leaks[i].kept)
            || !strstr(buf, "[SECRET_DETECTED:")) {
            printf("    row %zu: n=%d -> \"%s\"\n", i, n, buf);
            assert(0);
        }
    }
}

static void test_aws_key_offset(void) {
    const char *text = "here is AKIAIOSFODNN7EXAMPLE ok";
    ScanFinding f[8];
    int n = secret_scan(text, strlen(text), f, 8);
    assert(n >= 1);
    assert(strcmp(f[0].rule_id, "aws-access-token") == 0);
    assert(f[0].offset == 8);
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
}

static void test_placeholder_neighbour(void) {
    /* A secret NEXT to a placeholder is still caught. */
    const char *mixed = "{{SECRET:A_B2}} and AKIAIOSFODNN7EXAMPLE";
    ScanFinding f[8];
    int n = secret_scan(mixed, strlen(mixed), f, 8);
    assert(n == 1);
    assert(strcmp(f[0].rule_id, "aws-access-token") == 0);
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
}

static void test_entropy_calculation(void) {
    assert(secret_scan_entropy("aaaaaaaaaa", 10) < 0.01f);
    assert(secret_scan_entropy("aB3kL9mXp7qR2wNv", 17) > 3.0f);
}

static void fill_aws_keys(char *buf, size_t cap, int count) {
    int off = 0;
    buf[0] = '\0';
    for (int i = 0; i < count; i++)
        off += snprintf(buf + off, cap - (size_t)off,
                        "AKIA%cBCDEFGHIJKLMNOP ", 'A' + (i % 26));
}

static void test_saturation(void) {
    char buf[2048];
    ScanFinding f[SCAN_MAX_FINDINGS];

    /* Exactly SCAN_MAX_FINDINGS (32) is not saturated. */
    fill_aws_keys(buf, sizeof(buf), 32);
    assert(secret_scan(buf, strlen(buf), f, SCAN_MAX_FINDINGS) == 32);

    /* One more signals max+1, and redaction replaces the whole content. */
    fill_aws_keys(buf, sizeof(buf), 33);
    assert(secret_scan(buf, strlen(buf), f, SCAN_MAX_FINDINGS) == SCAN_MAX_FINDINGS + 1);
    size_t len = strlen(buf);
    assert(secret_scan_redact(buf, &len, sizeof(buf)) == 1);
    assert(strcmp(buf, "[SECRET_DETECTED:content_redacted]") == 0);
}

int main(void) {
    TEST_INIT();
    printf("test_secret_scan:\n");
    RUN(test_detect_table);
    RUN(test_clean_table);
    RUN(test_redact_table);
    RUN(test_aws_key_offset);
    RUN(test_private_key);
    RUN(test_private_key_truncated);
    RUN(test_fine_grained_pat_full);
    RUN(test_placeholder_neighbour);
    RUN(test_redact_zero_slack);
    RUN(test_redact_zero_slack_multi);
    RUN(test_entropy_calculation);
    RUN(test_saturation);
    return 0;
}
