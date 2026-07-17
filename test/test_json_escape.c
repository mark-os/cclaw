#include "json_escape.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "test_util.h"

/* ── json_unescape tests ─────────────────────────────────────────── */

static void test_unescape_surrogate_pair(void) {
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
    const char *src = "\\uD800";
    char buf[8];
    size_t n = json_unescape(buf, sizeof(buf), src, strlen(src));
    assert(n == 3);
    assert((unsigned char)buf[0] == 0xEF);
    assert((unsigned char)buf[1] == 0xBF);
    assert((unsigned char)buf[2] == 0xBD);
}

static void test_unescape_high_surrogate_non_low(void) {
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
    const char *src = "\\uDC00";
    char buf[8];
    size_t n = json_unescape(buf, sizeof(buf), src, strlen(src));
    assert(n == 3);
    assert((unsigned char)buf[0] == 0xEF);
    assert((unsigned char)buf[1] == 0xBF);
    assert((unsigned char)buf[2] == 0xBD);
}

static void test_unescape_back_to_back_pairs(void) {
    const char *src = "\\uD83D\\uDE00\\uD83D\\uDE00";
    char buf[16];
    size_t n = json_unescape(buf, sizeof(buf), src, strlen(src));
    assert(n == 8);
    assert((unsigned char)buf[0] == 0xF0 && (unsigned char)buf[4] == 0xF0);
}

int main(void) {
    TEST_INIT();
    test_unescape_surrogate_pair();
    test_unescape_surrogate_pair_then_char();
    test_unescape_isolated_high_surrogate();
    test_unescape_high_surrogate_non_low();
    test_unescape_isolated_low_surrogate();
    test_unescape_back_to_back_pairs();
    printf("test_json_escape: all tests passed\n");
    return 0;
}
