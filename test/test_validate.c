#include "validate.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_is_valid_agent_name(void) {
    /* Accept: single uppercase letter */
    assert(is_valid_agent_name("A"));

    /* Accept: PascalCase word */
    assert(is_valid_agent_name("Assistant"));

    /* Accept: PascalCase with trailing digits */
    assert(is_valid_agent_name("CodeReviewer2"));

    /* Accept: exactly 63 chars starting uppercase */
    char name63[64];
    name63[0] = 'Z';
    for (int i = 1; i < 63; i++) name63[i] = 'a';
    name63[63] = '\0';
    assert(is_valid_agent_name(name63));

    printf("  PASS: valid agent names accepted\n");

    /* Reject: NULL */
    assert(!is_valid_agent_name(NULL));

    /* Reject: empty string */
    assert(!is_valid_agent_name(""));

    /* Reject: lowercase start */
    assert(!is_valid_agent_name("assistant"));

    /* Reject: contains hyphen */
    assert(!is_valid_agent_name("code-reviewer"));

    /* Reject: contains underscore */
    assert(!is_valid_agent_name("Code_Reviewer"));

    /* Reject: starts with digit */
    assert(!is_valid_agent_name("1Agent"));

    /* Reject: contains space */
    assert(!is_valid_agent_name("Agent Name"));

    /* Reject: 64 chars (too long) */
    char name64[65];
    name64[0] = 'A';
    for (int i = 1; i < 64; i++) name64[i] = 'b';
    name64[64] = '\0';
    assert(!is_valid_agent_name(name64));

    printf("  PASS: invalid agent names rejected\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
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

    test_is_valid_agent_name();

    printf("all validate tests passed\n");
    return 0;
}
