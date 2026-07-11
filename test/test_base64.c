/* Test: base64_encode (RFC 4648 with padding) */
#define _POSIX_C_SOURCE 200809L
#include "util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect(const char *name, const unsigned char *in, size_t len,
                   const char *want) {
    char *got = base64_encode(in, len);
    if (!got || strcmp(got, want) != 0) {
        printf("  %s... FAIL: got '%s' want '%s'\n", name, got ? got : "NULL", want);
        exit(1);
    }
    printf("  %s... PASS\n", name);
    free(got);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_base64\n");

    /* RFC 4648 §10 test vectors — every padding case */
    expect("empty", (const unsigned char *)"", 0, "");
    expect("f", (const unsigned char *)"f", 1, "Zg==");
    expect("fo", (const unsigned char *)"fo", 2, "Zm8=");
    expect("foo", (const unsigned char *)"foo", 3, "Zm9v");
    expect("foob", (const unsigned char *)"foob", 4, "Zm9vYg==");
    expect("fooba", (const unsigned char *)"fooba", 5, "Zm9vYmE=");
    expect("foobar", (const unsigned char *)"foobar", 6, "Zm9vYmFy");

    /* Binary: NULs and high bytes survive (the whole point — OGG over the
     * C-string JS bridge). */
    const unsigned char bin[] = {0x00, 0xff, 0x10, 0x80, 0x00};
    expect("binary_nuls", bin, sizeof(bin), "AP8QgAA=");

    printf("All tests passed\n");
    return 0;
}
