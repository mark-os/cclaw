#include "unicode_normalize.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Strip s (NUL-terminated) and assert the result equals expect. */
static void check(const char *label, const char *s, const char *expect) {
    size_t out_len = 0;
    char *r = unicode_strip_invisible(s, strlen(s), &out_len);
    assert(r);
    assert(out_len == strlen(expect));
    assert(memcmp(r, expect, out_len + 1) == 0);
    printf("  PASS %s\n", label);
    free(r);
}

static void test_ascii_passthrough(void) {
    check("ascii_passthrough", "hello world", "hello world");
    check("empty", "", "");
}

static void test_zero_width(void) {
    /* U+200B..U+200F between a-b-c-d-e-f */
    check("zero_width",
          "a\xE2\x80\x8B" "b\xE2\x80\x8C" "c\xE2\x80\x8D" "d\xE2\x80\x8E" "e\xE2\x80\x8F" "f",
          "abcdef");
}

static void test_bidi_overrides(void) {
    /* U+202A, U+202C, U+202E */
    check("bidi_overrides",
          "x\xE2\x80\xAA" "y\xE2\x80\xAC" "z\xE2\x80\xAE" "w",
          "xyzw");
}

static void test_word_joiner_isolates(void) {
    /* U+2060 word joiner, U+2066..U+2069 isolates */
    check("word_joiner_isolates",
          "a\xE2\x81\xA0" "b\xE2\x81\xA6" "c\xE2\x81\xA7" "d\xE2\x81\xA8" "e\xE2\x81\xA9" "f",
          "abcdef");
}

static void test_bom(void) {
    /* BOM (U+FEFF = EF BB BF) at position 0 is kept */
    check("bom_at_0_kept", "\xEF\xBB\xBFhello", "\xEF\xBB\xBFhello");
    /* BOM mid-string is stripped */
    check("bom_mid_stripped", "he\xEF\xBB\xBFllo", "hello");
}

static void test_soft_hyphen(void) {
    /* U+00AD = C2 AD */
    check("soft_hyphen", "pass\xC2\xADword", "password");
}

static void test_variation_selectors(void) {
    /* U+FE00 = EF B8 80, U+FE0F = EF B8 8F */
    check("variation_selectors", "a\xEF\xB8\x80" "b\xEF\xB8\x8F" "c", "abc");
}

static void test_multibyte_preserved(void) {
    /* Legit multibyte text (é = C3 A9, 漢 = E6 BC A2, 😀 = F0 9F 98 80)
     * with an interleaved zero-width space. */
    check("multibyte_preserved",
          "\xC3\xA9\xE2\x80\x8B\xE6\xBC\xA2\xF0\x9F\x98\x80",
          "\xC3\xA9\xE6\xBC\xA2\xF0\x9F\x98\x80");
}

static void test_invalid_utf8_passthrough(void) {
    /* Lone continuation byte, truncated lead, 0xFF — all pass through. */
    const char raw[] = { 'a', (char)0x80, 'b', (char)0xE2, 'c',
                         (char)0xFF, 'd', '\0' };
    size_t out_len = 0;
    char *r = unicode_strip_invisible(raw, 7, &out_len);
    assert(r);
    assert(out_len == 7);
    assert(memcmp(r, raw, 8) == 0);
    printf("  PASS invalid_utf8_passthrough\n");
    free(r);

    /* Truncated U+200B (only 2 of 3 bytes) at end of input: pass through. */
    const char trunc[] = { 'x', (char)0xE2, (char)0x80, '\0' };
    r = unicode_strip_invisible(trunc, 3, &out_len);
    assert(r);
    assert(out_len == 3);
    assert(memcmp(r, trunc, 4) == 0);
    printf("  PASS truncated_seq_passthrough\n");
    free(r);
}

static void test_embedded_nul(void) {
    /* Length-based API: bytes after a NUL are still processed. */
    const char raw[] = { 'a', '\0', '\xE2', '\x80', '\x8B', 'b' };
    size_t out_len = 0;
    char *r = unicode_strip_invisible(raw, sizeof(raw), &out_len);
    assert(r);
    assert(out_len == 3);
    assert(r[0] == 'a' && r[1] == '\0' && r[2] == 'b');
    printf("  PASS embedded_nul\n");
    free(r);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_unicode_normalize:\n");
    test_ascii_passthrough();
    test_zero_width();
    test_bidi_overrides();
    test_word_joiner_isolates();
    test_bom();
    test_soft_hyphen();
    test_variation_selectors();
    test_multibyte_preserved();
    test_invalid_utf8_passthrough();
    test_embedded_nul();
    printf("  ALL PASSED\n");
    return 0;
}
