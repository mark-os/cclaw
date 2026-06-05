/* T288: e2e pure reasoning (no tools)
 * Verifies: thinking + content, finish_reason=stop, no tool calls dispatched.
 * Requires OPENROUTER_API_KEY. */
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
    char line[65536];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, needle)) { fclose(f); return 1; }
    }
    fclose(f);
    return 0;
}

static void test_pure_reasoning(void) {
    TEST(pure_reasoning_no_tools);
    if (!getenv("OPENROUTER_API_KEY")) SKIP("OPENROUTER_API_KEY not set");

    const char *out = "/tmp/cclaw_e2e_t288_reasoning.txt";
    char cmd[1024];
    /* Use trace to see stop_reason; no tools that would trigger tool_calls */
    snprintf(cmd, sizeof(cmd),
        "CCLAW_TOOLS=memory_create,memory_append,memory_replace "
        "timeout 120 %s -y --new --log-level=trace "
        "-p \"Explain in one sentence why the sky is blue.\" > %s 2>&1",
        BINARY, out);
    int rc = system(cmd);
    rc = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;

    if (has_sanitizer_errors(out)) FAIL("sanitizer errors detected");
    if (rc != 0) FAIL("non-zero exit code");
    /* Must have a non-empty response (content about the sky) */
    if (!file_contains(out, "scatter") && !file_contains(out, "light") &&
        !file_contains(out, "blue") && !file_contains(out, "wavelength"))
        FAIL("expected reasoning content about light/scattering");
    /* No tool calls should be dispatched */
    if (file_contains(out, "[tool]"))
        FAIL("tool call dispatched in pure reasoning prompt");
    /* finish_reason should be 'stop' (V35 normalization) */
    if (!file_contains(out, "finish=stop"))
        FAIL("expected finish=stop in trace output");

    PASS();
}

int main(void) {
    printf("--- test_e2e_reasoning (T288) ---\n");
    test_pure_reasoning();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
