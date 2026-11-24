#define _GNU_SOURCE
#include "test_run_tool_shell.h"
#include <sys/stat.h>
#include "test_util.h"

/* Verify namespace sandbox restricts filesystem writes outside workspace.
 * Drives shell_exec via the --run-tool re-exec path (production behavior). */

static char workspace[256];

static void setup_workspace(void) {
    snprintf(workspace, sizeof(workspace), "/tmp/cclaw_ns_test_%d", getpid());
    mkdir(workspace, 0755);
}

static void cleanup_workspace(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", workspace);
    system(cmd);
}

/* Test: writing inside workspace succeeds */
static void test_write_inside_workspace(void) {
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.workspace = workspace;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "echo hello > %s/test_file.txt && cat %s/test_file.txt",
        workspace, workspace);
    r.command = cmd;
    char *res = run_tool_shell(&r);
    assert(res != NULL);
    assert(strstr(res, "[exit 0]") != NULL);
    assert(strstr(res, "hello") != NULL);
    free(res);
}

/* Test: writing to /etc fails (read-only bind mount) */
static void test_write_system_dir_blocked(void) {
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.workspace = workspace;
    r.command = "touch /etc/cclaw_escape_test 2>&1; echo rc=$?";
    char *res = run_tool_shell(&r);
    assert(res != NULL);
    assert(strstr(res, "rc=1") != NULL || strstr(res, "Read-only") != NULL);
    free(res);
}

/* Test: reading system files still works (/ is ro, not invisible) */
static void test_read_system_files(void) {
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.workspace = workspace;
    r.command = "ls /bin/sh";
    char *res = run_tool_shell(&r);
    assert(res != NULL);
    assert(strstr(res, "[exit 0]") != NULL);
    assert(strstr(res, "/bin/sh") != NULL);
    free(res);
}

/* Test: network is isolated (CLONE_NEWNET = no interfaces except lo down) */
static void test_network_isolated(void) {
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.workspace = workspace;
    /* In a new network namespace, only lo exists and it's down.
     * Some kernels also create sit0 (IPv6-over-IPv4 tunnel) in new netns. */
    r.command = "cat /proc/net/dev | grep ':' | grep -v 'lo' | grep -v 'sit0' | wc -l";
    char *res = run_tool_shell(&r);
    assert(res != NULL);
    /* Should have 0 non-lo, non-sit0 interfaces */
    if (strstr(res, "[exit 0]")) {
        assert(strstr(res, "\n0\n") != NULL);
    }
    free(res);
}

/* Test: agent DB path is not accessible from shell child */
static void test_agent_db_inaccessible(void) {
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.workspace = workspace;
    r.command = "cat /root/.bashrc 2>&1; echo rc=$?";
    char *res = run_tool_shell(&r);
    assert(res != NULL);
    assert(strstr(res, "rc=1") != NULL || strstr(res, "No such file") != NULL);
    free(res);
}

/* Verify /etc/shadow inaccessible from shell child.
 *
 * This one is DAC, not containment: /etc is bind-mounted read-only, so the file
 * is reachable in the mount tree and only its mode keeps it unreadable. The
 * sandbox identity-maps the invoking uid (uid_map "<uid> <uid> 1"), so when
 * cclaw itself runs as uid 0 the payload is root and CAP_DAC_OVERRIDE applies
 * to root-owned files — the read succeeds and this assertion cannot hold. That is
 * the real cost of running as root (see specs/security.md), not a containment
 * failure: the read-only remounts and the empty netns hold either way. Skip
 * rather than fail, so a root-in-a-container dev box still gets a green suite. */
static void test_etc_shadow_inaccessible(void) {
    if (geteuid() == 0) {
        printf("    skipped: running as root, uid_map is identity\n");
        return;
    }
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.workspace = workspace;
    r.command = "cat /etc/shadow 2>&1; echo rc=$?";
    char *res = run_tool_shell(&r);
    assert(res != NULL);
    assert(strstr(res, "rc=1") != NULL || strstr(res, "Permission denied") != NULL
           || strstr(res, "No such file") != NULL);
    free(res);
}

/* Without cwd_path (daemon mode), CWD dir is NOT accessible */
static void test_no_cwd_path_blocked(void) {

    char cwd_dir[256];
    snprintf(cwd_dir, sizeof(cwd_dir), "/tmp/cclaw_nocwd_test_%d", getpid());
    mkdir(cwd_dir, 0755);
    char seed[512];
    snprintf(seed, sizeof(seed), "%s/seed.txt", cwd_dir);
    FILE *f = fopen(seed, "w");
    fprintf(f, "secret\n");
    fclose(f);

    /* No cwd_path — simulates daemon mode */
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.workspace = workspace;
    /* cwd_path defaults to NULL */

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "cat %s/seed.txt 2>&1; echo rc=$?", cwd_dir);
    r.command = cmd;
    char *res = run_tool_shell(&r);
    assert(res != NULL);
    assert(strstr(res, "rc=1") != NULL || strstr(res, "No such file") != NULL);
    free(res);

    char rm[512];
    snprintf(rm, sizeof(rm), "rm -rf %s", cwd_dir);
    system(rm);
}

/* Key/DB masking is covered in test_sandbox_key_mask.c. */
/* sandbox=0 (uncontained) — namespace NOT applied, child runs unsandboxed */
static void test_sandbox_none_skips_namespace(void) {
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.workspace = workspace;
    r.plan = TEST_PLAN_NONE;

    /* With sandbox=0, child should be able to write to /tmp directly (no pivot_root) */
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
        "echo sandboxnone > /tmp/cclaw_sbtest_%d && cat /tmp/cclaw_sbtest_%d", getpid(), getpid());
    r.command = cmd;
    char *res = run_tool_shell(&r);
    assert(res != NULL);
    assert(strstr(res, "[exit 0]") != NULL);
    assert(strstr(res, "sandboxnone") != NULL);
    free(res);

    /* Cleanup */
    char rm[128];
    snprintf(rm, sizeof(rm), "rm -f /tmp/cclaw_sbtest_%d", getpid());
    system(rm);
}

/* Nothing under /dev that the host shares is writable from inside: a file
 * the child creates in /dev/shm or /dev/mqueue must not appear on the host.
 * Recursive binds carried the host's /dev submounts in read-write — the top
 * remount did not reach them — so both were a channel between agents and to
 * the host (fails on 8a6ba35). The child still gets working, private ones:
 * the write must succeed inside, or the row proves nothing. */
static void test_host_dev_not_shared(void) {
    static const char *dirs[] = { "/dev/shm", "/dev/mqueue" };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        struct stat st;
        if (stat(dirs[i], &st) != 0) {
            printf("  SKIP %s (absent on this host)\n", dirs[i]);
            continue;
        }
        char name[128], cmd[512];
        snprintf(name, sizeof(name), "%s/cclaw_ns_probe_%d", dirs[i], getpid());
        unlink(name);
        snprintf(cmd, sizeof(cmd), "touch %s && echo WROTE", name);
        ShellToolReq r = SHELL_REQ_DEFAULTS;
        r.workspace = workspace;
        r.command = cmd;
        char *res = run_tool_shell(&r);
        assert(res != NULL);
        int host_sees = access(name, F_OK) == 0;
        unlink(name);
        if (host_sees) printf("  host sees %s\n", name);
        assert(!host_sees);
        assert(strstr(res, "WROTE") != NULL);
        free(res);
    }
}

/* A sandboxed child runs with no_new_privs, so a setuid binary the payload
 * runs does not elevate (fails on 8a6ba35, where it was never set). Containment
 * none does not set it: it keeps the operator's own reach, sudo included. */
static int g_ns_ok;
static void test_no_new_privs(void) {
    static const struct { int sandbox; const char *want; } cases[] = {
        { 1, "NoNewPrivs:\t1" }, { 0, "NoNewPrivs:\t0" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (cases[i].sandbox && !g_ns_ok) {
            printf("  SKIP no_new_privs sandboxed row (namespaces unavailable)\n");
            continue;
        }
        ShellToolReq r = SHELL_REQ_DEFAULTS;
        r.workspace = workspace;
        r.plan = cases[i].sandbox ? TEST_PLAN_STANDARD : TEST_PLAN_NONE;
        r.command = "grep NoNewPrivs /proc/self/status";
        char *res = run_tool_shell(&r);
        assert(res != NULL);
        if (!strstr(res, cases[i].want)) printf("  row %zu: %s\n", i, res);
        assert(strstr(res, cases[i].want) != NULL);
        free(res);
    }
}

/* An agent folder deep enough that <dir>/.proxy.<pid>.sock exceeds sun_path
 * (108 bytes) still gets its proxy. The bind used to fail and the child ran
 * with no proxy variables and no word why (fails on 8a6ba35). */
static void test_deep_agent_dir_proxy(void) {
    char deep[256];
    int n = snprintf(deep, sizeof(deep), "%s/", workspace);
    while (n < 120) n += snprintf(deep + n, sizeof(deep) - (size_t)n, "deep_agent_dir/");
    deep[n - 1] = '\0';
    char mk[512];
    snprintf(mk, sizeof(mk), "mkdir -p %s", deep);
    assert(system(mk) == 0);

    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.workspace = workspace;
    r.agent_dir = deep;
    r.command = "echo \"P=[$HTTP_PROXY]\"; test -S /tmp/.cclaw_proxy.sock && echo SOCK";
    char *res = run_tool_shell(&r);
    assert(res != NULL);
    assert(strstr(res, "P=[http://127.0.0.1:") != NULL);
    assert(strstr(res, "SOCK") != NULL);
    free(res);
}

int main(void) {
    TEST_INIT();
    printf("test_shell_namespace:\n");
    setup_workspace();
    g_ns_ok = run_tool_ns_available(workspace);
    if (!g_ns_ok) {
        printf("  SKIP (namespaces unavailable on this system)\n");
    } else {
        RUN(test_write_inside_workspace);
        RUN(test_write_system_dir_blocked);
        RUN(test_read_system_files);
        RUN(test_network_isolated);
        RUN(test_agent_db_inaccessible);
        RUN(test_etc_shadow_inaccessible);
        RUN(test_no_cwd_path_blocked);
        RUN(test_host_dev_not_shared);
        RUN(test_deep_agent_dir_proxy);
    }
    RUN(test_sandbox_none_skips_namespace);
    RUN(test_no_new_privs);
    cleanup_workspace();
    return 0;
}
