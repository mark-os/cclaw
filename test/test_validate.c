#include "validate.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "test_util.h"

/* Names reach paths (workspaces, channel pipes) and SQL identifiers, so
 * every rejected shape stays a row. */
static void test_names(void) {
    char n63[64], n64[65], a63[64], a64[65];
    memset(n63, 'a', 63); n63[63] = '\0';
    memset(n64, 'a', 64); n64[64] = '\0';
    a63[0] = 'Z'; memset(a63 + 1, 'a', 62); a63[63] = '\0';
    a64[0] = 'A'; memset(a64 + 1, 'b', 63); a64[64] = '\0';

    /* agent: 1 = is_valid_agent_name, 0 = is_valid_name */
    const struct { int agent; const char *name; int ok; } cases[] = {
        {0, "hello", 1}, {0, "Hello", 1}, {0, "AGENT", 1}, {0, "my-agent", 1},
        {0, "my_agent", 1}, {0, "Agent123", 1}, {0, "a", 1}, {0, "A-B_c-9", 1},
        {0, n63, 1},
        {0, NULL, 0}, {0, "", 0}, {0, "has space", 0}, {0, "has.dot", 0},
        {0, "path/sep", 0}, {0, "back\\slash", 0}, {0, "tab\there", 0},
        {0, "new\nline", 0}, {0, "@special", 0}, {0, "no!bang", 0}, {0, n64, 0},
        /* agent names are PascalCase, letters and digits only */
        {1, "A", 1}, {1, "Assistant", 1}, {1, "CodeReviewer2", 1}, {1, a63, 1},
        {1, NULL, 0}, {1, "", 0}, {1, "assistant", 0}, {1, "code-reviewer", 0},
        {1, "Code_Reviewer", 0}, {1, "1Agent", 0}, {1, "Agent Name", 0}, {1, a64, 0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int got = cases[i].agent ? is_valid_agent_name(cases[i].name)
                                 : is_valid_name(cases[i].name);
        if (!!got != cases[i].ok)
            fprintf(stderr, "row %zu: '%s' -> %d\n", i,
                    cases[i].name ? cases[i].name : "(null)", got);
        assert(!!got == cases[i].ok);
    }
}

int main(void) {
    TEST_INIT();
    printf("test_validate:\n");
    RUN(test_names);
    printf("all validate tests passed\n");
    return 0;
}
