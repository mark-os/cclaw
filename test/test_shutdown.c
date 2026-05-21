#include "shutdown.h"
#include <stdio.h>
#include <signal.h>

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { printf("FAIL: %s\n", msg); } \
    else { tests_passed++; } \
} while(0)

static void test_initial_state(void) {
    shutdown_reset();
    ASSERT(shutdown_requested() == 0, "initially not requested");
}

static void test_request(void) {
    shutdown_reset();
    shutdown_request();
    ASSERT(shutdown_requested() != 0, "requested after shutdown_request()");
}

static void test_reset(void) {
    shutdown_request();
    shutdown_reset();
    ASSERT(shutdown_requested() == 0, "reset clears flag");
}

static void test_signal_handler(void) {
    shutdown_reset();
    shutdown_init();
    /* Deliver SIGINT to ourselves */
    raise(SIGINT);
    ASSERT(shutdown_requested() != 0, "SIGINT sets shutdown flag");
    shutdown_reset();
}

int main(void) {
    test_initial_state();
    test_request();
    test_reset();
    test_signal_handler();

    printf("%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
