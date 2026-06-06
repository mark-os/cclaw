#define _POSIX_C_SOURCE 200809L
#include "agent_exit.h"
#include <assert.h>
#include <stdio.h>

/* Verify exit code constants */
static void test_exit_codes(void) {
    assert(AGENT_EXIT_DONE == 0);
    assert(AGENT_EXIT_ERROR == 1);
    printf("  PASS: exit code constants\n");
}

int main(void) {
    printf("test_agent_exit:\n");
    test_exit_codes();
    printf("ALL PASSED\n");
    return 0;
}
