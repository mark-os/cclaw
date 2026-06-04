/* T285: e2e multi-tool chain — file_write → shell_exec (python3) → verify output
 * Verifies: tool dispatch chain works, file_write creates file, shell_exec runs it,
 * no ASAN/UBSan errors, exit 0. Requires OPENROUTER_API_KEY. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define BINARY "./build/cclaw"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)
#define SKIP(msg) do { tests_passed++; printf("SKIP: %s\n", msg); return; } while(0)

static int has_sanitizer_errors(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "ERROR: AddressSanitizer") ||
            strstr(line, "ERROR: LeakSanitizer") ||
            strstr(line, "runtime error:"))
        { fclose(f); return 1; }
    }
    fclose(f);
    return 0;
}

static int file_contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, needle)) { fclose(f); return 1; }
    }
    fclose(f);
    return 0;
}

static void test_multi_tool_chain(void) {
    TEST(file_write_then_shell_exec);
    if (!getenv("OPENROUTER_API_KEY")) SKIP("OPENROUTER_API_KEY not set");

    /* Clean up any leftover from previous run */
    unlink("/tmp/cclaw_hello.py");

    const char *out = "/tmp/cclaw_e2e_t285_multitools.txt";
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "timeout 120 %s -y --new --log-level=trace "
        "-p \"Write a Python script to /tmp/cclaw_hello.py that prints 'hello from cclaw', "
        "then run it with python3 and show me the output\" > %s 2>&1",
        BINARY, out);
    int rc = system(cmd);
    rc = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;

    if (has_sanitizer_errors(out)) FAIL("sanitizer errors detected");
    if (rc != 0) FAIL("non-zero exit code");
    /* Verify both tools were dispatched */
    if (!file_contains(out, "file_write")) FAIL("file_write tool not dispatched");
    if (!file_contains(out, "shell_exec")) FAIL("shell_exec tool not dispatched");
    /* Verify the script was actually created */
    if (access("/tmp/cclaw_hello.py", F_OK) != 0) FAIL("/tmp/cclaw_hello.py not created");
    /* Verify output contains the expected string */
    if (!file_contains(out, "hello from cclaw")) FAIL("expected 'hello from cclaw' in output");

    /* Cleanup */
    unlink("/tmp/cclaw_hello.py");
    PASS();
}

int main(void) {
    printf("--- test_e2e_multitools (T285) ---\n");
    test_multi_tool_chain();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
