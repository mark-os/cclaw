#include "json_escape.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "test_util.h"

/* json_unescape on \u escapes: a valid surrogate pair becomes one 4-byte
 * UTF-8 character; any half pair becomes U+FFFD (EF BF BD) and the bytes
 * after it are still decoded. */
static void test_unescape_surrogates(void) {
    static const struct { const char *src; size_t n; const char *want; } cases[] = {
        { "\\uD83D\\uDE00", 4, "\xF0\x9F\x98\x80" },
        { "\\uD83D\\uDE00A", 5, "\xF0\x9F\x98\x80" "A" },
        { "\\uD800", 3, "\xEF\xBF\xBD" },                     /* isolated high */
        { "\\uD800\\u0041", 4, "\xEF\xBF\xBD" "A" },          /* high, then not a low */
        { "\\uDC00", 3, "\xEF\xBF\xBD" },                     /* isolated low */
        { "\\uD83D\\uDE00\\uD83D\\uDE00", 8, "\xF0\x9F\x98\x80\xF0\x9F\x98\x80" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char buf[16];
        size_t n = json_unescape(buf, sizeof(buf), cases[i].src, strlen(cases[i].src));
        if (n != cases[i].n || memcmp(buf, cases[i].want, n) != 0) {
            fprintf(stderr, "row %zu (%s): got %zu bytes, want %zu\n", i, cases[i].src, n, cases[i].n);
            assert(0);
        }
    }
}

int main(void) {
    TEST_INIT();
    RUN(test_unescape_surrogates);
    printf("test_json_escape: all tests passed\n");
    return 0;
}
