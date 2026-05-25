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
    int rc = tool_shell_register(&reg, 0, NULL, 0, NULL, 0);
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

/* V47: Verify PATH is restricted and env is clean */
static void test_env_hardened(void) {
    /* Set vars that should be stripped */
    setenv("OPENROUTER_API_KEY", "secret123", 1);
    setenv("GEMINI_API_KEY", "secret456", 1);
    setenv("CCLAW_DB_PATH", "/tmp/test.db", 1);
    setenv("CCLAW_WEB_PORT", "8080", 1);

    char *r = tool_shell_handler("{\"command\":\"env\"}", NULL);
    assert(r != NULL);
    assert(strstr(r, "[exit 0]") != NULL);
    /* API keys must not appear */
    assert(strstr(r, "OPENROUTER_API_KEY") == NULL);
    assert(strstr(r, "GEMINI_API_KEY") == NULL);
    assert(strstr(r, "CCLAW_DB_PATH") == NULL);
    assert(strstr(r, "CCLAW_WEB_PORT") == NULL);
    /* HOME must not appear */
    assert(strstr(r, "\nHOME=") == NULL);
    /* PATH must be restricted */
    assert(strstr(r, "PATH=/bin:/usr/bin") != NULL);
    free(r);

    /* Clean up parent env */
    unsetenv("OPENROUTER_API_KEY");
    unsetenv("GEMINI_API_KEY");
    unsetenv("CCLAW_DB_PATH");
    unsetenv("CCLAW_WEB_PORT");
    printf("  PASS test_env_hardened\n");
}

/* V47: Verify cclaw binary is unreachable via PATH */
static void test_cclaw_unreachable(void) {
    char *r = tool_shell_handler("{\"command\":\"which cclaw 2>/dev/null; echo rc=$?\"}", NULL);
    assert(r != NULL);
    /* which should fail (rc=1) since cclaw is in build/ not /bin or /usr/bin */
    assert(strstr(r, "rc=1") != NULL);
    free(r);
    printf("  PASS test_cclaw_unreachable\n");
}

/* V49: Test that MJS_FETCH_FD is set in child environment */
static void test_fetch_fd_set(void) {
    ShellConfig sc = {.timeout = 5, .workspace = NULL, .shell_network = 0,
                      .allowed_hosts = NULL, .allowed_hosts_count = 0};
    char *r = tool_shell_handler("{\"command\":\"echo MJS_FETCH_FD=$MJS_FETCH_FD\"}", &sc);
    assert(r != NULL);
    assert(strstr(r, "[exit 0]") != NULL);
    /* MJS_FETCH_FD should be set to a valid fd number */
    assert(strstr(r, "MJS_FETCH_FD=") != NULL);
    /* Should NOT be empty */
    assert(strstr(r, "MJS_FETCH_FD=\n") == NULL);
    free(r);
    printf("  PASS test_fetch_fd_set\n");
}

/* V49/V50: Test fetch proxy end-to-end with mjs binary.
 * mjs sends fetch request over fd, parent proxies it.
 * Since no real HTTP server, we test the denied-host path. */
static void test_fetch_proxy_denied(void) {
    /* No allowed_hosts → all fetches denied */
    ShellConfig sc = {.timeout = 5, .workspace = NULL, .shell_network = 0,
                      .allowed_hosts = NULL, .allowed_hosts_count = 0};
    char *r = tool_shell_handler(
        "{\"command\":\"./build/mjs -e 'var r = http_fetch(\\\"http://example.com/x\\\"); r.error'\"}",
        &sc);
    assert(r != NULL);
    assert(strstr(r, "[exit 0]") != NULL);
    assert(strstr(r, "no allowed_hosts") != NULL);
    free(r);
    printf("  PASS test_fetch_proxy_denied\n");
}

/* V49: Test fetch proxy with allowed host (will fail to connect but passes policy) */
static void test_fetch_proxy_allowed_host(void) {
    char *hosts[] = {"httpbin.org"};
    ShellConfig sc = {.timeout = 5, .workspace = NULL, .shell_network = 0,
                      .allowed_hosts = hosts, .allowed_hosts_count = 1};
    /* Host is allowed but network is blocked (CLONE_NEWNET) or unreachable.
     * The key test: policy check passes (no "not in allowed_hosts" error).
     * We'll get a curl error instead. */
    char *r = tool_shell_handler(
        "{\"command\":\"./build/mjs -e 'var r = http_fetch(\\\"http://httpbin.org/get\\\"); r.error || r.status'\"}",
        &sc);
    assert(r != NULL);
    assert(strstr(r, "[exit 0]") != NULL);
    /* Should NOT get "not in allowed_hosts" — policy passed */
    assert(strstr(r, "not in allowed_hosts") == NULL);
    assert(strstr(r, "no allowed_hosts") == NULL);
    free(r);
    printf("  PASS test_fetch_proxy_allowed_host\n");
}

/* V49: Test fetch proxy rejects host not in allowlist */
static void test_fetch_proxy_host_rejected(void) {
    char *hosts[] = {"api.github.com"};
    ShellConfig sc = {.timeout = 5, .workspace = NULL, .shell_network = 0,
                      .allowed_hosts = hosts, .allowed_hosts_count = 1};
    char *r = tool_shell_handler(
        "{\"command\":\"./build/mjs -e 'var r = http_fetch(\\\"http://evil.com/x\\\"); r.error'\"}",
        &sc);
    assert(r != NULL);
    assert(strstr(r, "[exit 0]") != NULL);
    assert(strstr(r, "not in allowed_hosts") != NULL);
    free(r);
    printf("  PASS test_fetch_proxy_host_rejected\n");
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
    test_env_hardened();
    test_cclaw_unreachable();
    test_fetch_fd_set();
    test_fetch_proxy_denied();
    test_fetch_proxy_allowed_host();
    test_fetch_proxy_host_rejected();
    printf("All shell_exec tool tests passed.\n");
    return 0;
}
