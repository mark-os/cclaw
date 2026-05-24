#define _GNU_SOURCE
#include "tool_shell.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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

static void test_timeout_kills_process_group(void) {
    char pidfile[] = "/tmp/cclaw_test_pgkill_XXXXXX";
    int fd = mkstemp(pidfile);
    assert(fd >= 0);
    close(fd);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "{\"command\":\"sh -c 'echo $$ > %s; sleep 60'\",\"timeout\":1}", pidfile);

    char *r = tool_shell_handler(cmd, NULL);
    assert(r != NULL);
    assert(strstr(r, "[timeout after 1s]") != NULL);
    free(r);

    struct timespec ts = {0, 100000000};
    nanosleep(&ts, NULL);
    FILE *f = fopen(pidfile, "r");
    if (f) {
        int child_pid = 0;
        if (fscanf(f, "%d", &child_pid) == 1 && child_pid > 0) {
            assert(kill(child_pid, 0) != 0);
        }
        fclose(f);
    }
    unlink(pidfile);
    printf("  PASS test_timeout_kills_process_group\n");
}

static void test_configurable_default_timeout(void) {
    ShellConfig sc = {.timeout = 1, .workspace = NULL, .shell_network = 0};
    char *r = tool_shell_handler("{\"command\":\"sleep 10\"}", &sc);
    assert(r != NULL);
    assert(strstr(r, "[timeout after 1s]") != NULL);
    free(r);
    printf("  PASS test_configurable_default_timeout\n");
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
    int rc = tool_shell_register(&reg, 0, NULL, 0);
    assert(rc == 0);
    ToolEntry *e = tools_lookup(&reg, "shell_exec");
    assert(e != NULL);
    assert(e->handler == tool_shell_handler);
    tools_free(&reg);
    printf("  PASS test_register\n");
}

/* V37: Test that namespace sandbox applies gracefully (fallback on EPERM) */
static void test_namespace_sandbox_fallback(void) {
    /* Run with workspace set — unshare may fail without CAP_SYS_ADMIN,
     * but command should still execute (graceful fallback) */
    ShellConfig sc = {.timeout = 5, .workspace = "/tmp", .shell_network = 0};
    char *r = tool_shell_handler("{\"command\":\"echo sandboxed\"}", &sc);
    assert(r != NULL);
    assert(strstr(r, "sandboxed") != NULL);
    free(r);
    printf("  PASS test_namespace_sandbox_fallback\n");
}

/* V37: Test shell_network flag doesn't break execution */
static void test_shell_network_flag(void) {
    ShellConfig sc = {.timeout = 5, .workspace = "/tmp", .shell_network = 1};
    char *r = tool_shell_handler("{\"command\":\"echo netok\"}", &sc);
    assert(r != NULL);
    assert(strstr(r, "netok") != NULL);
    free(r);
    printf("  PASS test_shell_network_flag\n");
}

int main(void) {
    printf("test_tool_shell:\n");
    test_basic_command();
    test_stderr_captured();
    test_nonzero_exit();
    test_timeout();
    test_timeout_kills_process_group();
    test_configurable_default_timeout();
    test_invalid_json();
    test_missing_command();
    test_register();
    test_namespace_sandbox_fallback();
    test_shell_network_flag();
    printf("All shell_exec tool tests passed.\n");
    return 0;
}
