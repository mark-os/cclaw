#include "validate.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    printf("test_validate:\n");

    /* Valid names */
    assert(is_valid_name("hello"));
    assert(is_valid_name("Hello"));
    assert(is_valid_name("AGENT"));
    assert(is_valid_name("my-agent"));
    assert(is_valid_name("my_agent"));
    assert(is_valid_name("Agent123"));
    assert(is_valid_name("a"));
    assert(is_valid_name("A-B_c-9"));
    printf("  PASS: valid names accepted\n");

    /* Invalid names */
    assert(!is_valid_name(NULL));
    assert(!is_valid_name(""));
    assert(!is_valid_name("has space"));
    assert(!is_valid_name("has.dot"));
    assert(!is_valid_name("path/sep"));
    assert(!is_valid_name("back\\slash"));
    assert(!is_valid_name("tab\there"));
    assert(!is_valid_name("new\nline"));
    assert(!is_valid_name("@special"));
    assert(!is_valid_name("no!bang"));
    printf("  PASS: invalid names rejected\n");

    /* Too long (64 chars) */
    char long_name[65];
    memset(long_name, 'a', 64);
    long_name[64] = '\0';
    assert(!is_valid_name(long_name));

    /* Exactly 63 chars is ok */
    long_name[63] = '\0';
    assert(is_valid_name(long_name));
    printf("  PASS: length limits enforced\n");

    printf("all validate tests passed\n");
    return 0;
}
