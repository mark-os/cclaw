/* test_integration_shell_proxy_deny.c — what a shell child is told when the
 * proxy denies a host. Uses the real --run-tool broker path (fork+execve). */
#define _GNU_SOURCE
#include "test_run_tool_shell.h"
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "test_util.h"

static char workspace[256];
static int ns_available = 0;

/* The broker takes its egress allowlist as data — no grant DB. */
static const char *allow_named[] = {"allowed.test"};

static void setup_workspace(void) {
    snprintf(workspace, sizeof(workspace), "/tmp/cclaw_proxy_deny_%d", getpid());
    mkdir(workspace, 0755);
}

static void cleanup_workspace(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", workspace);
    system(cmd);
}

static void check_prerequisites(void) {
    struct stat st;
    if (stat("./build/net_shim", &st) != 0) {
        printf("  SKIP: net_shim not built\n");
        exit(0);
    }
    ns_available = run_tool_ns_available(workspace);
}

/* A narrowed call's deny summary carries the parent-composed binding note in
 * place of the generic grants.hosts advice; without a note the generic line
 * still renders (runtime-bind-discovery, D17). */
static void test_denied_summary_advice(void) {
    if (!ns_available) {
        printf("  SKIP test_denied_summary_advice (namespaces unavailable)\n");
        return;
    }

    /* An https CONNECT is denied by the allowlist before any DNS. (An
     * in-namespace loopback connect never leaves the netns at all, so it
     * records no attributable denial.) */
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = "curl -s --max-time 5 https://denied.test/";
    r.workspace = workspace;
    r.timeout = 10;
    r.plan = TEST_PLAN_STANDARD;
    r.host_rules = allow_named;
    r.host_count = 1;
    r.egress_note = "this call ran narrowed to its secrets' bound hosts "
                    "(ALPACA_KEY -> allowed.test) — request the pair via "
                    "request_config (secret_bindings), then re-issue";

    char *result = run_tool_shell(&r);
    assert(result != NULL);
    assert(strstr(result, "proxy blocked host(s) not in allowlist: denied.test"));
    assert(strstr(result, "ALPACA_KEY -> allowed.test"));
    assert(strstr(result, "secret_bindings"));
    assert(!strstr(result, "grants.hosts"));   /* note replaces the advice */
    free(result);

    /* Without a note: the generic advice still stands. */
    r.egress_note = NULL;
    result = run_tool_shell(&r);
    assert(result != NULL);
    assert(strstr(result, "proxy blocked host(s) not in allowlist: denied.test"));
    assert(strstr(result, "grants.hosts"));
    free(result);
    printf("  PASS test_denied_summary_advice\n");
}

int main(void) {
    TEST_INIT();
    printf("test_integration_shell_proxy_deny:\n");

    setup_workspace();
    check_prerequisites();
    test_denied_summary_advice();
    cleanup_workspace();

    printf("All shell proxy deny tests passed.\n");
    return 0;
}
