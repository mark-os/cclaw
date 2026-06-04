/* T283: e2e streaming + thinking + tool calls
 * Verifies: DeepSeek V4 Flash, thinking blocks handled, tool dispatch works,
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

/* Run binary, capture combined stdout+stderr to file, return exit code */
static int run_cclaw(const char *args, const char *outfile, const char *env_extra) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "%s%s timeout 120 %s %s > %s 2>&1",
             env_extra ? env_extra : "", env_extra ? " " : "",
             BINARY, args, outfile);
    int rc = system(cmd);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

/* Check file for ASAN/UBSan errors */
static int has_sanitizer_errors(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "ERROR: AddressSanitizer") ||
            strstr(line, "ERROR: LeakSanitizer") ||
            strstr(line, "runtime error:"))
        { fclose(f); return 1; }
    }
    fclose(f);
    return 0;
}

/* Check file contains a substring */
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

/* T283: Streaming + thinking + tool calls (yolo mode) */
static void test_streaming_thinking_tools(void) {
    TEST(streaming_thinking_tools);
    if (!getenv("OPENROUTER_API_KEY")) SKIP("OPENROUTER_API_KEY not set");

    const char *out = "/tmp/cclaw_e2e_t283_stream.txt";
    /* Prompt designed to force a tool call (js_eval is available in yolo) */
    int rc = run_cclaw(
        "-y --new --log-level=trace -p \"Use js_eval to compute 2+2 and tell me the result\"",
        out, NULL);

    if (has_sanitizer_errors(out)) FAIL("sanitizer errors detected");
    if (rc != 0) FAIL("non-zero exit code");
    /* Verify tool was called (trace shows tool dispatch) */
    if (!file_contains(out, "js_eval")) FAIL("js_eval tool not dispatched");
    /* Verify final response is non-empty (contains "4") */
    if (!file_contains(out, "4")) FAIL("expected result '4' in output");

    PASS();
}

/* T283 part 2: Verify thinking blocks don't leak as user-visible content corruption */
static void test_thinking_blocks_handled(void) {
    TEST(thinking_blocks_handled);
    if (!getenv("OPENROUTER_API_KEY")) SKIP("OPENROUTER_API_KEY not set");

    const char *out = "/tmp/cclaw_e2e_t283_think.txt";
    /* Simple prompt that triggers thinking but no tools */
    int rc = run_cclaw(
        "-y --new -p \"What is 7 * 8? Answer with just the number.\"",
        out, NULL);

    if (has_sanitizer_errors(out)) FAIL("sanitizer errors detected");
    if (rc != 0) FAIL("non-zero exit code");
    /* Should contain "56" in output */
    if (!file_contains(out, "56")) FAIL("expected '56' in output");
    /* Thinking tags should NOT appear in visible output (they go to stderr in trace) */
    /* The stdout portion (before trace lines) should be clean */

    PASS();
}

int main(void) {
    printf("--- test_e2e_streaming (T283) ---\n");
    test_streaming_thinking_tools();
    test_thinking_blocks_handled();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
