/* Child-side spill: a tool whose output exceeds RUNTOOL_RESULT_MAX writes the
 * whole thing to the parent-resolved spill path and returns only the head.
 * Drives the real fork+execve --run-tool path. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "test_run_tool_shell.h"
#include "test_util.h"

static const char *WS = "/tmp/test_spill_child_ws";

static void test_spill(void) {
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
    assert(result);
    assert(strlen(result) <= RUNTOOL_RESULT_MAX + 512);
    assert(strstr(result, spill));
    assert(strstr(result, "truncated"));

    /* The file holds the WHOLE output, not what the wire would allow. */
    struct stat st;
    assert(stat(spill, &st) == 0);
    assert(st.st_size > RUNTOOL_RESULT_MAX);
    assert(st.st_size > 400000);

    char grep[768];
    snprintf(grep, sizeof(grep), "grep -q 'line 19999' %s", spill);
    assert(system(grep) == 0);
    assert(!strstr(result, "line 19999"));
    free(result);

    /* Without a spill path the result is still capped, just unsaved. */
    r.spill_path = NULL;
    result = run_tool_shell(&r);
    assert(result && strlen(result) <= 300 * 1024);
    free(result);
}

int main(void) {
    TEST_INIT();
    printf("test_spill_child:\n");
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", WS, WS);
    assert(system(cmd) == 0);
    RUN(test_spill);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", WS);
    if (system(cmd) != 0) { /* best effort */ }
    return 0;
}
