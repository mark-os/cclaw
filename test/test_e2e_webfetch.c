/* T287: e2e web_fetch — fetch httpbin.org, verify content extraction.
 * Cites: V46 (http_policy allows in host mode (--trust-host)). Requires OPENROUTER_API_KEY. */
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

static void test_web_fetch_httpbin(void) {
    TEST(web_fetch_httpbin);
    if (!getenv("OPENROUTER_API_KEY")) SKIP("OPENROUTER_API_KEY not set");

    const char *out = "/tmp/cclaw_e2e_t287_webfetch.txt";
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "timeout 120 %s --trust-host --new --log-level=trace "
        "-p \"Fetch https://httpbin.org/json and tell me what's in the response\" "
        "> %s 2>&1",
        BINARY, out);
    int rc = system(cmd);
    rc = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;

    if (has_sanitizer_errors(out)) FAIL("sanitizer errors detected");
    if (rc != 0) FAIL("non-zero exit code");
    if (!file_contains(out, "web_fetch")) FAIL("web_fetch tool not dispatched");
    /* httpbin.org/json returns a JSON object with a "slideshow" key */
    if (!file_contains(out, "slideshow")) FAIL("expected 'slideshow' content from httpbin");
    PASS();
}

int main(void) {
    printf("--- test_e2e_webfetch (T287) ---\n");
    test_web_fetch_httpbin();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
