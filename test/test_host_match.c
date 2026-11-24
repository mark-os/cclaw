#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include "host_match.h"
#include "test_util.h"

enum { MATCH, COVERED };

/* host_match (allow rules) and host_covered (deny labels). */
static const struct {
    int fn; int n; const char *rules[2]; const char *host; int want;
} hosts[] = {
    {MATCH, 2, {"github.com", "api.example.com"}, "github.com", 1},
    {MATCH, 2, {"github.com", "api.example.com"}, "api.example.com", 1},
    {MATCH, 2, {"github.com", "api.example.com"}, "evil.com", 0},
    {MATCH, 1, {".github.com"}, "api.github.com", 1},
    {MATCH, 1, {".github.com"}, "deep.api.github.com", 1},
    {MATCH, 1, {".github.com"}, "github.com", 1},          /* suffix covers base */
    /* the dot boundary is load-bearing */
    {MATCH, 1, {".github.com"}, "evilgithub.com", 0},
    {MATCH, 1, {".github.com"}, "notgithub.com", 0},
    /* the fully-qualified spelling names the same host, allow and deny */
    {MATCH, 2, {".github.com", "example.org"}, "api.github.com.", 1},
    {MATCH, 2, {".github.com", "example.org"}, "example.org.", 1},
    {MATCH, 2, {".github.com", "example.org"}, "evilgithub.com.", 0},
    {COVERED, 1, {"secret.example.com"}, "secret.example.com.", 1},
    {COVERED, 1, {"secret.example.com"}, "a.secret.example.com.", 1},
    /* ASCII fold regardless of process locale (review-3 F2) */
    {MATCH, 2, {"GitHub.com", ".Example.COM"}, "github.com", 1},
    {MATCH, 2, {"GitHub.com", ".Example.COM"}, "API.EXAMPLE.com", 1},
    {MATCH, 0, {NULL}, "x.com", 0},                        /* no rules: deny */
    {MATCH, 1, {"*"}, "anything.example", 1},
    {MATCH, 1, {"*"}, "a", 1},
    /* "*.example.com" is not the wildcard sentinel and globs nothing */
    {MATCH, 1, {"*.example.com"}, "api.example.com", 0},
    {MATCH, 1, {"*.example.com"}, "example.com", 0},
};

static void test_host_table(void) {
    for (size_t i = 0; i < sizeof(hosts) / sizeof(hosts[0]); i++) {
        char **r = (char **)hosts[i].rules;
        int got = hosts[i].fn == MATCH ? host_match(r, hosts[i].n, hosts[i].host)
                                       : host_covered(r, hosts[i].n, hosts[i].host);
        if (!!got != hosts[i].want) {
            printf("    row %zu: %s -> %d, want %d\n", i, hosts[i].host, got, hosts[i].want);
            assert(0);
        }
    }
}

/* host_in_text: registered-domain semantics over free text. */
static const struct {
    int n; const char *label; const char *text; int want; const char *token;
} texts[] = {
    {1, "chase.com", "{\"url\":\"https://chase.com/login\"}", 1, "chase.com"},
    {1, "chase.com", "curl -s api.chase.com/transfer", 1, "api.chase.com"},
    {1, "chase.com", "visit mychase.com now", 0, NULL},
    {1, "chase.com", "https://chase.com.evil.co/x", 0, NULL},
    {1, "chase.com", "chasexcom or chase-com", 0, NULL},
    {1, "bank.example", "go to BANK.EXAMPLE.", 1, "BANK.EXAMPLE"},
    {1, "bank.example", "", 0, NULL},
    {0, "bank.example", "bank.example", 0, NULL},
    {1, ".pay.example.com", "POST https://api.pay.example.com/v1", 1, NULL},
    {1, ".pay.example.com", "pay.example.com", 1, NULL},
    {1, ".pay.example.com", "prepay.example.com", 0, NULL},
};

static void test_text_table(void) {
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
        char *labels[1] = {(char *)texts[i].label};
        char got[254] = "";
        int hit = host_in_text(labels, texts[i].n, texts[i].text, got, sizeof(got));
        if (!!hit != texts[i].want ||
            (texts[i].token && strcmp(got, texts[i].token) != 0)) {
            printf("    row %zu: \"%s\" -> %d \"%s\"\n", i, texts[i].text, hit, got);
            assert(0);
        }
    }
}

/* cidr_parse: host bits zeroed, bare IP is a full-length prefix, and a
 * malformed prefix must never default to /0 (allow-all). */
static const struct {
    const char *in; int ok; int family; int plen; unsigned char bytes[16];
} parses[] = {
    {"192.168.1.0/24", 1, AF_INET, 24, {192, 168, 1, 0}},
    {"10.0.0.5", 1, AF_INET, 32, {10, 0, 0, 5}},
    {"192.168.1.100/24", 1, AF_INET, 24, {192, 168, 1, 0}},
    {"fe80::/10", 1, AF_INET6, 10, {0xfe, 0x80}},
    {"::1", 1, AF_INET6, 128, {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1}},
    {"10.0.0.0/", 0, 0, 0, {0}},
    {"1.2.3.4/xyz", 0, 0, 0, {0}},
    {"1.2.3.4/33", 0, 0, 0, {0}},
    {"fe80::/129", 0, 0, 0, {0}},
};

static void test_parse_table(void) {
    for (size_t i = 0; i < sizeof(parses) / sizeof(parses[0]); i++) {
        Cidr c;
        int ok = cidr_parse(parses[i].in, &c) == 0;
        int good = ok == parses[i].ok;
        if (good && ok)
            good = c.family == parses[i].family && c.prefix_len == parses[i].plen &&
                   memcmp(c.prefix, parses[i].bytes,
                          parses[i].family == AF_INET ? 4 : 16) == 0;
        if (!good) {
            printf("    row %zu: %s\n", i, parses[i].in);
            assert(0);
        }
    }
}

enum { CIDR_MATCH, EXACT };

/* cidr_match, and granted_exact: only an exact /32 or /128 grant counts,
 * so a covering CIDR cannot open the metadata carve-out. */
static const struct {
    int fn; int n; const char *rules[2]; const char *addr; int want;
} matches[] = {
    {CIDR_MATCH, 1, {"192.168.1.0/24"}, "192.168.1.50", 1},
    {CIDR_MATCH, 1, {"192.168.1.0/24"}, "192.168.2.50", 0},
    {CIDR_MATCH, 1, {"10.0.0.5"}, "10.0.0.5", 1},
    {CIDR_MATCH, 1, {"10.0.0.5"}, "10.0.0.6", 0},
    {CIDR_MATCH, 1, {"fe80::/10"}, "fe80::1", 1},
    {CIDR_MATCH, 1, {"fe80::/10"}, "2001:db8::1", 0},
    {CIDR_MATCH, 1, {"::1"}, "::1", 1},
    {CIDR_MATCH, 1, {"::1"}, "::2", 0},
    {EXACT, 2, {"169.254.0.0/16", "169.254.169.254"}, "169.254.169.254", 1},
    {EXACT, 1, {"169.254.0.0/16"}, "169.254.169.254", 0},
    {EXACT, 1, {"0.0.0.0/0"}, "169.254.169.254", 0},
};

static void test_match_table(void) {
    for (size_t i = 0; i < sizeof(matches) / sizeof(matches[0]); i++) {
        Cidr rules[2];
        for (int k = 0; k < matches[i].n; k++)
            assert(cidr_parse(matches[i].rules[k], &rules[k]) == 0);
        int fam = strchr(matches[i].addr, ':') ? AF_INET6 : AF_INET;
        unsigned char a[16];
        assert(inet_pton(fam, matches[i].addr, a) == 1);
        int got = matches[i].fn == CIDR_MATCH ? cidr_match(rules, matches[i].n, fam, a)
                                              : granted_exact(rules, matches[i].n, fam, a);
        if (!!got != matches[i].want) {
            printf("    row %zu: %s -> %d\n", i, matches[i].addr, got);
            assert(0);
        }
    }
}

/* url_host_normalize (r2 F11): encoded or invisible-char spellings of a
 * host must reach the deny check as the plain host. */
static const struct { const char *in; const char *out; } norms[] = {
    {"chase%2Ecom", "chase.com"},
    {"chase%252Ecom", "chase.com"},             /* %25 -> '%', then %2E -> '.' */
    {"chase\xE2\x80\x8B.com", "chase.com"},     /* U+200B inside the host */
    {"api.example.com", "api.example.com"},
};

static void test_normalize_table(void) {
    for (size_t i = 0; i < sizeof(norms) / sizeof(norms[0]); i++) {
        char host[254];
        snprintf(host, sizeof(host), "%s", norms[i].in);
        url_host_normalize(host, sizeof(host));
        char *labels[1] = {(char *)norms[i].out};
        if (strcmp(host, norms[i].out) != 0 || !host_covered(labels, 1, host)) {
            printf("    row %zu: -> \"%s\"\n", i, host);
            assert(0);
        }
    }
}

int main(void) {
    TEST_INIT();
    printf("test_host_match\n");
    RUN(test_host_table);
    RUN(test_text_table);
    RUN(test_parse_table);
    RUN(test_match_table);
    RUN(test_normalize_table);
    return 0;
}
