#define _POSIX_C_SOURCE 200809L
#include "tool_shell.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_basic_command(void) {
    char *r = tool_shell_handler("{\"command\":\"echo hello\"}", NULL);
    assert(r != NULL);
    assert(strstr(r, "[exit 0]") != NULL);
    assert(strstr(r, "hello") != NULL);
    free(r);
    printf("  PASS test_basic_command\n");
}

static void test_stderr_captured(void) {
    char *r = tool_shell_handler("{\"command\":\"echo err >&2\"}", NULL);
    assert(r != NULL);
    assert(strstr(r, "[exit 0]") != NULL);
    assert(strstr(r, "err") != NULL);
    free(r);
    printf("  PASS test_stderr_captured\n");
}

static void test_nonzero_exit(void) {
    char *r = tool_shell_handler("{\"command\":\"exit 42\"}", NULL);
    assert(r != NULL);
    assert(strstr(r, "[exit 42]") != NULL);
    free(r);
    printf("  PASS test_nonzero_exit\n");
}

static void test_timeout(void) {
    char *r = tool_shell_handler("{\"command\":\"sleep 10\",\"timeout\":1}", NULL);
    assert(r != NULL);
    assert(strstr(r, "[timeout after 1s]") != NULL);
    free(r);
    printf("  PASS test_timeout\n");
}

static void test_invalid_json(void) {
    char *r = tool_shell_handler("not json", NULL);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL);
    free(r);
    printf("  PASS test_invalid_json\n");
}

static void test_missing_command(void) {
    char *r = tool_shell_handler("{\"timeout\":5}", NULL);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL);
    free(r);
    printf("  PASS test_missing_command\n");
}

static void test_register(void) {
    ToolRegistry reg;
    tools_init(&reg);
    int rc = tool_shell_register(&reg);
    assert(rc == 0);
    ToolEntry *e = tools_lookup(&reg, "shell_exec");
    assert(e != NULL);
    assert(e->handler == tool_shell_handler);
    tools_free(&reg);
    printf("  PASS test_register\n");
}

int main(void) {
    printf("test_tool_shell:\n");
    test_basic_command();
    test_stderr_captured();
    test_nonzero_exit();
    test_timeout();
    test_invalid_json();
    test_missing_command();
    test_register();
    printf("All shell_exec tool tests passed.\n");
    return 0;
}
