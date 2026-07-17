/* test_integration_shell_timeout.c — P5 #9: timeout cleanup across the PID
 * namespace.
 *
 * Open design question (specs/shell-networking.md): when a sandboxed command
 * times out, the broker issues kill(-pid, SIGKILL) on the OUTER sandbox process
 * in the host PID namespace. The actual command runs as PID 1 of a child PID
 * namespace (CLONE_NEWPID); any background job it spawned and the net_shim are
 * further descendants inside that namespace. Does the host-side process-group
 * kill — plus PID-1-death namespace teardown — actually reap them all?
 *
 * We drive a real timeout: the command starts a background loop that appends a
 * heartbeat to ./hb (the workspace is bind-mounted at its real path and the
 * command chdir's there, so the host sees the file grow) and then hangs. After
 * the tool reports the timeout, we record hb's size, wait, and re-check: if it
 * stopped growing, every descendant was reaped. A still-growing file means a
 * survivor crossed the PID namespace — a real defect to report, not paper over.
 *
 * ns-gated: skips when unprivileged user namespaces are unavailable.
 *
 * Uses the --run-tool broker path (fork+execve of the real binary), not the
 * in-process tool_shell_handler().
 */
#define _GNU_SOURCE
#include "test_run_tool_shell.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "test_util.h"

static char workspace[256];
static int ns_available = 1;

/* Allow loopback so the per-call broker + net_shim are stood up (keeps the shim
 * in the process tree, which is part of what we're checking gets reaped). */
static const char *host_rules[] = {"127.0.0.1"};

static void setup_workspace(void) {
    snprintf(workspace, sizeof(workspace), "/tmp/cclaw_timeout_%d", getpid());
    mkdir(workspace, 0755);
}

static void cleanup_workspace(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", workspace);
    (void)!system(cmd);
}

static void check_prerequisites(void) {
    struct stat st;
    if (stat("./build/libcclaw_net.so", &st) != 0 || stat("./build/net_shim", &st) != 0) {
        printf("  SKIP: libcclaw_net.so / net_shim not built\n");
        exit(0);
    }
    if (stat(SHELL_BINARY, &st) != 0) {
        printf("  SKIP: %s not built\n", SHELL_BINARY);
        exit(0);
    }
    ns_available = run_tool_ns_available(workspace);
}

static long file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static void msleep(long ms) {
    nanosleep(&(struct timespec){.tv_sec = ms / 1000,
                                 .tv_nsec = (ms % 1000) * 1000000}, NULL);
}

static void test_timeout_reaps_namespace(void) {
    if (!ns_available) {
        printf("  SKIP test_timeout_reaps_namespace (no namespaces)\n");
        return;
    }

    /* Background heartbeat writer + a hung foreground. The writer is a child of
     * PID 1 (the command's /bin/sh) inside the new PID namespace. */
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = "(while :; do echo x >> ./hb; sleep 0.2; done) & "
                "echo STARTED; sleep 30";
    r.workspace = workspace;
    r.timeout = 2;
    r.host_rules = host_rules;
    r.host_count = 1;

    char *result = run_tool_shell(&r);
    assert(result != NULL);
    /* The broker only returns after kill(-pid)+waitpid, i.e. cleanup is done. */
    assert(strstr(result, "[timeout") && "command should have timed out");
    assert(strstr(result, "STARTED") && "command should have started");
    free(result);

    char hb[512];
    snprintf(hb, sizeof(hb), "%s/hb", workspace);
    long sz1 = file_size(hb);
    /* The writer must have produced output before the timeout fired. */
    assert(sz1 > 0 && "heartbeat writer never ran");

    /* If any descendant survived the cleanup, hb keeps growing. Wait well past
     * the 0.2s heartbeat interval and compare. */
    msleep(1500);
    long sz2 = file_size(hb);

    if (sz2 != sz1) {
        printf("  FAIL test_timeout_reaps_namespace\n");
        printf("  heartbeat kept growing after cleanup: %ld -> %ld bytes\n", sz1, sz2);
        printf("  a descendant survived kill(-pid) across the PID namespace\n");
        assert(0 && "survivor across PID namespace — see report");
    }
    printf("  PASS test_timeout_reaps_namespace (hb frozen at %ld bytes)\n", sz1);
}

int main(void) {
    TEST_INIT();
    signal(SIGPIPE, SIG_IGN);
    printf("test_integration_shell_timeout (P5 #9):\n");
    setup_workspace();
    check_prerequisites();
    test_timeout_reaps_namespace();
    cleanup_workspace();
    printf("All timeout-cleanup tests passed.\n");
    return 0;
}
