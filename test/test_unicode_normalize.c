#include "unicode_normalize.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_util.h"

/* A literal with its exact byte length, so embedded NULs count. */
#define S(x) x, sizeof(x) - 1

static const struct {
    const char *in; size_t in_len; const char *out; size_t out_len;
} cases[] = {
    {S("hello world"), S("hello world")},
    {S(""), S("")},
    /* U+200B..U+200F */
    {S("a\xE2\x80\x8B" "b\xE2\x80\x8C" "c\xE2\x80\x8D" "d\xE2\x80\x8E" "e\xE2\x80\x8F" "f"),
     S("abcdef")},
    /* bidi overrides U+202A, U+202C, U+202E */
    {S("x\xE2\x80\xAA" "y\xE2\x80\xAC" "z\xE2\x80\xAE" "w"), S("xyzw")},
    /* U+2060 word joiner, U+2066..U+2069 isolates */
    {S("a\xE2\x81\xA0" "b\xE2\x81\xA6" "c\xE2\x81\xA7" "d\xE2\x81\xA8" "e\xE2\x81\xA9" "f"),
     S("abcdef")},
    /* BOM kept at position 0, stripped mid-string */
    {S("\xEF\xBB\xBFhello"), S("\xEF\xBB\xBFhello")},
    {S("he\xEF\xBB\xBFllo"), S("hello")},
    {S("pass\xC2\xADword"), S("password")},                       /* soft hyphen */
    {S("a\xEF\xB8\x80" "b\xEF\xB8\x8F" "c"), S("abc")},            /* variation selectors */
    /* legit multibyte (é, 漢, 😀) around a zero-width space */
    {S("\xC3\xA9\xE2\x80\x8B\xE6\xBC\xA2\xF0\x9F\x98\x80"),
     S("\xC3\xA9\xE6\xBC\xA2\xF0\x9F\x98\x80")},
    /* invalid UTF-8 passes through: lone continuation, truncated lead, 0xFF */
    {S("a\x80" "b\xE2" "c\xFF" "d"), S("a\x80" "b\xE2" "c\xFF" "d")},
    /* truncated U+200B (2 of 3 bytes) at end of input */
    {S("x\xE2\x80"), S("x\xE2\x80")},
    /* length-based API: bytes after a NUL are still processed */
    {S("a\0\xE2\x80\x8B" "b"), S("a\0b")},
};

static void test_strip_table(void) {
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t out_len = 0;
        char *r = unicode_strip_invisible(cases[i].in, cases[i].in_len, &out_len);
        assert(r);
        if (out_len != cases[i].out_len ||
            memcmp(r, cases[i].out, out_len) != 0 || r[out_len] != '\0') {
            printf("    row %zu: out_len %zu, want %zu\n", i, out_len, cases[i].out_len);
            assert(0);
        }
        free(r);
    }
}

int main(void) {
    TEST_INIT();
    printf("test_unicode_normalize:\n");
    RUN(test_strip_table);
    return 0;
}
