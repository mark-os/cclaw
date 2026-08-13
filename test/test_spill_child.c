/* Child-side spill: a tool whose output exceeds RUNTOOL_RESULT_MAX writes the
 * whole thing to the parent-resolved spill path and returns only the head.
 * Drives the real fork+execve --run-tool path. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "test_run_tool_shell.h"
#include "test_util.h"

static int tests_run = 0, tests_passed = 0;
#define CHECK(name, cond) do { tests_run++; printf("  %s... ", name); \
    if (cond) { tests_passed++; printf("PASS\n"); } else printf("FAIL\n"); } while (0)

static const char *WS = "/tmp/test_spill_child_ws";

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    TEST_INIT();
    printf("test_spill_child:\n");
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", WS, WS);
    if (system(cmd) != 0) { printf("setup failed\n"); return 1; }

    char spill[512];
    snprintf(spill, sizeof(spill), "%s/big.out", WS);

    /* ~200KB of output: over RUNTOOL_RESULT_MAX and over the line cap. */
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = "i=0; while [ $i -lt 20000 ]; do echo \"line $i padding padding\"; i=$((i+1)); done";
    r.workspace = WS;
    r.spill_path = spill;
    r.sandbox = 0;
    r.net_mode = 1;
    char *result = run_tool_shell(&r);
    CHECK("tool returned a result", result != NULL);
    if (!result) return 1;

    size_t rlen = strlen(result);
    CHECK("result fits the wire cap", rlen <= RUNTOOL_RESULT_MAX + 512);
    CHECK("result names the spill file", strstr(result, spill) != NULL);
    CHECK("result says truncated", strstr(result, "truncated") != NULL);

    struct stat st;
    int have = (stat(spill, &st) == 0);
    CHECK("spill file exists", have);
    /* The point of the refactor: the file holds the WHOLE output, not the
     * 64KB the wire would have allowed. */
    CHECK("spill file larger than the wire cap", have && st.st_size > RUNTOOL_RESULT_MAX);
    CHECK("spill file has every line", have && st.st_size > 400000);

    /* last line present in the file, absent from the truncated result */
    char grep[768];
    snprintf(grep, sizeof(grep), "grep -q 'line 19999' %s", spill);
    CHECK("spill file ends with the last line", system(grep) == 0);
    CHECK("result does NOT contain the last line", strstr(result, "line 19999") == NULL);
    free(result);

    /* Without a spill path the result is still capped, just unsaved. */
    r.spill_path = NULL;
    result = run_tool_shell(&r);
    CHECK("no spill path: still bounded", result && strlen(result) <= 300 * 1024);
    free(result);

    snprintf(cmd, sizeof(cmd), "rm -rf %s", WS);
    if (system(cmd) != 0) { /* best effort */ }
    printf("test_spill_child: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
