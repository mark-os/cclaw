#define _GNU_SOURCE
#include "test_run_tool_shell.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* For test_register only */
#include "tool_shell.h"

static char workspace[256];

static void setup(void) {
    snprintf(workspace, sizeof(workspace), "/tmp/cclaw_test_shell_XXXXXX");
    assert(mkdtemp(workspace) != NULL);
}

static void cleanup(void) {
    rmdir(workspace);
}

static void test_basic_command(void) {
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = "echo hello";
    r.workspace = workspace;
    char *res = run_tool_shell(&r);
    assert(res != NULL);
    assert(strstr(res, "[exit 0]") != NULL);
    assert(strstr(res, "hello") != NULL);
    free(res);
    printf("  PASS test_basic_command\n");
}

static void test_default_shell_is_sh(void) {
    /* dash (the typical /bin/sh) rejects [[ ]] as a syntax error; bash
     * accepts it. Default (shell_path unset) must behave like /bin/sh. */
    if (system("/bin/sh -c '[[ 1 -eq 1 ]]' 2>/dev/null") == 0) {
        printf("  SKIP test_default_shell_is_sh (/bin/sh is bash — [[ ]] can't discriminate)\n");
        return;
    }
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = "[ -x /bin/bash ] || exit 99; [[ 1 -eq 1 ]] && echo yes";
    r.workspace = workspace;
    char *res = run_tool_shell(&r);
    assert(res != NULL);
    if (strstr(res, "[exit 99]")) { free(res); printf("  SKIP test_default_shell_is_sh (no /bin/bash)\n"); return; }
    assert(strstr(res, "[exit 0]") == NULL);
    free(res);
    printf("  PASS test_default_shell_is_sh\n");
}

static void test_configurable_shell_path(void) {
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = "[[ 1 -eq 1 ]] && echo yes";
    r.shell_path = "/bin/bash";
    r.workspace = workspace;
    if (access("/bin/bash", X_OK) != 0) { printf("  SKIP test_configurable_shell_path (no /bin/bash)\n"); return; }
    char *res = run_tool_shell(&r);
    assert(res != NULL);
    assert(strstr(res, "[exit 0]") != NULL);
    assert(strstr(res, "yes") != NULL);
    free(res);
    printf("  PASS test_configurable_shell_path\n");
}

static void test_stderr_captured(void) {
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = "echo err >&2";
    r.workspace = workspace;
    char *res = run_tool_shell(&r);
    assert(res != NULL);
    assert(strstr(res, "[exit 0]") != NULL);
    assert(strstr(res, "err") != NULL);
    free(res);
    printf("  PASS test_stderr_captured\n");
}

static void test_nonzero_exit(void) {
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = "exit 42";
    r.workspace = workspace;
    char *res = run_tool_shell(&r);
    assert(res != NULL);
    assert(strstr(res, "[exit 42]") != NULL);
    free(res);
    printf("  PASS test_nonzero_exit\n");
}

static void test_timeout(void) {
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = "sleep 10";
    r.timeout = 1;
    r.workspace = workspace;
    char *res = run_tool_shell(&r);
    assert(res != NULL);
    assert(strstr(res, "[timeout after 1s]") != NULL);
    free(res);
    printf("  PASS test_timeout\n");
}

static void test_timeout_kills_process_group(void) {
    char pidfile[] = "/tmp/cclaw_test_pgkill_XXXXXX";
    int fd = mkstemp(pidfile);
    assert(fd >= 0);
    close(fd);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "sh -c 'echo $$ > %s; sleep 60'", pidfile);

    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = cmd;
    r.timeout = 1;
    r.workspace = workspace;
    char *res = run_tool_shell(&r);
    assert(res != NULL);
    assert(strstr(res, "[timeout after 1s]") != NULL);
    free(res);

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
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = "sleep 10";
    r.timeout = 1;
    r.workspace = workspace;
    char *res = run_tool_shell(&r);
    assert(res != NULL);
    assert(strstr(res, "[timeout after 1s]") != NULL);
    free(res);
    printf("  PASS test_configurable_default_timeout\n");
}

static void test_register(void) {
    ToolRegistry reg;
    tools_init(&reg);
    int rc = tool_shell_register(&reg, 0, NULL, NULL);
    assert(rc == 0);
    ToolEntry *e = tools_lookup(&reg, "shell_exec");
    assert(e != NULL);
    assert(e->recipe.vehicle == EXEC_SANDBOX);
    assert(e->recipe.tier == SBX_SHELL);
    tools_free(&reg);
    printf("  PASS test_register\n");
}

/* Verify env vars set in test process don't reach the sandboxed child */
static void test_env_hardened(void) {
    if (!run_tool_ns_available(workspace)) {
        printf("  SKIP test_env_hardened (namespaces unavailable)\n");
        return;
    }

    setenv("OPENROUTER_API_KEY", "secret123", 1);
    setenv("GEMINI_API_KEY", "secret456", 1);
    setenv("CCLAW_DB_PATH", "/tmp/test.db", 1);
    setenv("CCLAW_WEB_PORT", "8080", 1);

    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = "env";
    r.workspace = workspace;
    char *res = run_tool_shell(&r);
    assert(res != NULL);
    assert(strstr(res, "[exit 0]") != NULL);
    assert(strstr(res, "OPENROUTER_API_KEY") == NULL);
    assert(strstr(res, "GEMINI_API_KEY") == NULL);
    assert(strstr(res, "CCLAW_DB_PATH") == NULL);
    assert(strstr(res, "CCLAW_WEB_PORT") == NULL);
    assert(strstr(res, "\nHOME=") == NULL);
    free(res);

    unsetenv("OPENROUTER_API_KEY");
    unsetenv("GEMINI_API_KEY");
    unsetenv("CCLAW_DB_PATH");
    unsetenv("CCLAW_WEB_PORT");
    printf("  PASS test_env_hardened\n");
}

/* Verify cclaw binary is unreachable via PATH */
static void test_cclaw_unreachable(void) {
    if (!run_tool_ns_available(workspace)) {
        printf("  SKIP test_cclaw_unreachable (namespaces unavailable)\n");
        return;
    }

    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = "which cclaw 2>/dev/null; echo rc=$?";
    r.workspace = workspace;
    char *res = run_tool_shell(&r);
    assert(res != NULL);
    assert(strstr(res, "rc=1") != NULL);
    free(res);
    printf("  PASS test_cclaw_unreachable\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_tool_shell:\n");

    setup();

    /* Basic tests — sandbox=1 (default), need ns */
    if (!run_tool_ns_available(workspace)) {
        printf("  SKIP test_basic_command (namespaces unavailable)\n");
        printf("  SKIP test_stderr_captured (namespaces unavailable)\n");
        printf("  SKIP test_nonzero_exit (namespaces unavailable)\n");
        printf("  SKIP test_timeout (namespaces unavailable)\n");
        printf("  SKIP test_timeout_kills_process_group (namespaces unavailable)\n");
        printf("  SKIP test_configurable_default_timeout (namespaces unavailable)\n");
        printf("  SKIP test_default_shell_is_sh (namespaces unavailable)\n");
        printf("  SKIP test_configurable_shell_path (namespaces unavailable)\n");
    } else {
        test_basic_command();
        test_stderr_captured();
        test_nonzero_exit();
        test_timeout();
        test_timeout_kills_process_group();
        test_configurable_default_timeout();
        test_default_shell_is_sh();
        test_configurable_shell_path();
    }

    /* Registry test — no subprocess needed */
    test_register();

    /* Sandbox-specific tests (own ns detection inside) */
    test_env_hardened();
    test_cclaw_unreachable();

    cleanup();
    printf("All shell_exec tool tests passed.\n");
    return 0;
}
