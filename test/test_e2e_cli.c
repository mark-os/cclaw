/* Test -p single-turn CLI mode */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include "test_util.h"

#define BINARY "./build/cclaw"
#define DB_PATH "/tmp/test_cclaw_cli_p.sqlite"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)
#define SKIP(msg) do { tests_passed++; printf("SKIP: %s\n", msg); return; } while(0)

static void test_p_flag_produces_output(void) {
    TEST(p_flag_produces_output);
    if (!getenv("OPENROUTER_API_KEY")) SKIP("OPENROUTER_API_KEY not set");

    test_db_clean(DB_PATH);
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        BINARY " --new -p 'Reply with exactly: PONG' 2>/dev/null");

    FILE *fp = popen(cmd, "r");
    if (!fp) FAIL("popen failed");

    char buf[4096] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    int status = pclose(fp);

    if (WEXITSTATUS(status) != 0) FAIL("non-zero exit");
    if (n == 0) FAIL("no output");
    if (!strstr(buf, "PONG")) FAIL("expected PONG in output");

    PASS();
}

static void test_p_flag_no_arg(void) {
    TEST(p_flag_no_arg);

    char cmd[256];
    snprintf(cmd, sizeof(cmd), BINARY " -p 2>&1");
    FILE *fp = popen(cmd, "r");
    if (!fp) FAIL("popen failed");

    char buf[256] = {0};
    fread(buf, 1, sizeof(buf) - 1, fp);
    int status = pclose(fp);

    if (WEXITSTATUS(status) == 0) FAIL("should fail with no arg");
    if (!strstr(buf, "requires")) FAIL("expected error message about -p requiring arg");

    PASS();
}

int main(void) {
    TEST_INIT();
    printf("--- test_integration_cli ---\n");
    test_p_flag_produces_output();
    test_p_flag_no_arg();
    test_db_clean(DB_PATH);
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
