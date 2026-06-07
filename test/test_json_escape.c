#include "json_escape.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

static void test_basic_string(void) {
    char buf[64];
    size_t n = json_escape_into(buf, sizeof(buf), "hello");
    assert(n == 5);
    assert(strcmp(buf, "hello") == 0);
}

static void test_quotes_and_backslash(void) {
    char buf[64];
    size_t n = json_escape_into(buf, sizeof(buf), "say \"hi\" \\ done");
    assert(n == 18);
    assert(strcmp(buf, "say \\\"hi\\\" \\\\ done") == 0);
}

static void test_newline_cr_tab(void) {
    char buf[64];
    size_t n = json_escape_into(buf, sizeof(buf), "a\nb\rc\td");
    assert(n == 10);
    assert(strcmp(buf, "a\\nb\\rc\\td") == 0);
}

static void test_control_chars(void) {
    char buf[64];
    /* \x01 and \x1f are control chars → \u0001, \u001f */
    size_t n = json_escape_into(buf, sizeof(buf), "\x01\x1f");
    assert(n == 12);
    assert(strcmp(buf, "\\u0001\\u001f") == 0);
}

static void test_null_src(void) {
    char buf[8] = "garbage";
    size_t n = json_escape_into(buf, sizeof(buf), NULL);
    assert(n == 0);
    assert(buf[0] == '\0');
}

static void test_empty_string(void) {
    char buf[8];
    size_t n = json_escape_into(buf, sizeof(buf), "");
    assert(n == 0);
    assert(buf[0] == '\0');
}

static void test_buffer_overflow(void) {
    char buf[4];
    /* "abcdef" needs 6 bytes — buffer only has 4 */
    size_t n = json_escape_into(buf, sizeof(buf), "abcdef");
    assert(n == 6); /* returns required size */
    assert(buf[3] == '\0'); /* NUL-terminated at cap-1 */
    assert(buf[0] == 'a' && buf[1] == 'b' && buf[2] == 'c');
}

static void test_overflow_with_escape(void) {
    char buf[4];
    /* "\n" escapes to "\\n" (2 chars), "x" = 1 char → total 3 needed + NUL */
    size_t n = json_escape_into(buf, sizeof(buf), "\nx");
    assert(n == 3);
    assert(strcmp(buf, "\\nx") == 0);

    /* Now overflow: "\n\n" = 4 chars needed, buf has cap 4 → fits exactly? No, NUL needs space */
    char buf2[4];
    n = json_escape_into(buf2, sizeof(buf2), "\n\n");
    assert(n == 4); /* 4 bytes needed, cap=4 means last byte is NUL */
    assert(buf2[3] == '\0');
}

static void test_zero_cap(void) {
    size_t n = json_escape_into(NULL, 0, "test");
    assert(n == 4); /* returns required size even with zero cap */
}

/* ── json_unescape tests ─────────────────────────────────────────── */

static void test_unescape_surrogate_pair(void) {
    /* \uD83D\uDE00 → U+1F600 (😀) = F0 9F 98 80 in UTF-8 */
    const char *src = "\\uD83D\\uDE00";
    char buf[8];
    size_t n = json_unescape(buf, sizeof(buf), src, strlen(src));
    assert(n == 4);
    assert((unsigned char)buf[0] == 0xF0);
    assert((unsigned char)buf[1] == 0x9F);
    assert((unsigned char)buf[2] == 0x98);
    assert((unsigned char)buf[3] == 0x80);
}

static void test_unescape_surrogate_pair_then_char(void) {
    /* Verify no character is skipped after a surrogate pair */
    const char *src = "\\uD83D\\uDE00A";
    char buf[8];
    size_t n = json_unescape(buf, sizeof(buf), src, strlen(src));
    assert(n == 5);
    assert((unsigned char)buf[0] == 0xF0);
    assert((unsigned char)buf[1] == 0x9F);
    assert((unsigned char)buf[2] == 0x98);
    assert((unsigned char)buf[3] == 0x80);
    assert(buf[4] == 'A');
}

static void test_unescape_isolated_high_surrogate(void) {
    /* \uD800 alone → U+FFFD = EF BF BD */
    const char *src = "\\uD800";
    char buf[8];
    size_t n = json_unescape(buf, sizeof(buf), src, strlen(src));
    assert(n == 3);
    assert((unsigned char)buf[0] == 0xEF);
    assert((unsigned char)buf[1] == 0xBF);
    assert((unsigned char)buf[2] == 0xBD);
}

static void test_unescape_high_surrogate_non_low(void) {
    /* \uD800\u0041 → U+FFFD then 'A' (0x41 is not a low surrogate) */
    const char *src = "\\uD800\\u0041";
    char buf[8];
    size_t n = json_unescape(buf, sizeof(buf), src, strlen(src));
    assert(n == 4);
    assert((unsigned char)buf[0] == 0xEF);
    assert((unsigned char)buf[1] == 0xBF);
    assert((unsigned char)buf[2] == 0xBD);
    assert(buf[3] == 'A');
}

static void test_unescape_isolated_low_surrogate(void) {
    /* \uDC00 alone → U+FFFD */
    const char *src = "\\uDC00";
    char buf[8];
    size_t n = json_unescape(buf, sizeof(buf), src, strlen(src));
    assert(n == 3);
    assert((unsigned char)buf[0] == 0xEF);
    assert((unsigned char)buf[1] == 0xBF);
    assert((unsigned char)buf[2] == 0xBD);
}

static void test_unescape_back_to_back_pairs(void) {
    /* Two surrogate pairs back to back */
    const char *src = "\\uD83D\\uDE00\\uD83D\\uDE00";
    char buf[16];
    size_t n = json_unescape(buf, sizeof(buf), src, strlen(src));
    assert(n == 8);
    assert((unsigned char)buf[0] == 0xF0 && (unsigned char)buf[4] == 0xF0);
}

int main(void) {
    test_basic_string();
    test_quotes_and_backslash();
    test_newline_cr_tab();
    test_control_chars();
    test_null_src();
    test_empty_string();
    test_buffer_overflow();
    test_overflow_with_escape();
    test_zero_cap();
    test_unescape_surrogate_pair();
    test_unescape_surrogate_pair_then_char();
    test_unescape_isolated_high_surrogate();
    test_unescape_high_surrogate_non_low();
    test_unescape_isolated_low_surrogate();
    test_unescape_back_to_back_pairs();
    printf("test_json_escape: all tests passed\n");
    return 0;
}
