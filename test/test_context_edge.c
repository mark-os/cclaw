#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "context.h"
#include "test_util.h"

static void test_session_tmp_dir(void) {
    char buf[64];
    session_tmp_dir(42, buf, sizeof(buf));
    assert(strcmp(buf, "/tmp/cclaw-42") == 0);
    printf("  PASS test_session_tmp_dir\n");
}

int main(void) {
    TEST_INIT();
    alarm(10);
    printf("test_context_edge:\n");
    test_session_tmp_dir();
    printf("All context_edge tests passed.\n");
    return 0;
}
