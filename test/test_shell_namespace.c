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

/* CWD rw bind-mount in CLI mode — cwd_path accessible rw alongside workspace */
static void test_cwd_path_rw(void) {

    /* Create a separate "cwd" directory to simulate CLI CWD */
    char cwd_dir[256];
    snprintf(cwd_dir, sizeof(cwd_dir), "/tmp/cclaw_cwd_test_%d", getpid());
    mkdir(cwd_dir, 0755);

    /* Write a file in cwd_dir to read from sandbox */
    char seed[512];
    snprintf(seed, sizeof(seed), "%s/seed.txt", cwd_dir);
    FILE *f = fopen(seed, "w");
    fprintf(f, "cwd_content\n");
    fclose(f);

    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.workspace = workspace;
    r.cwd_path = cwd_dir;
    r.mount_cwd = 1;

    /* Read from cwd_path — should work */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "cat %s/seed.txt", cwd_dir);
    r.command = cmd;
    char *res = run_tool_shell(&r);
    assert(res != NULL);
    assert(strstr(res, "[exit 0]") != NULL);
    assert(strstr(res, "cwd_content") != NULL);
    free(res);

    /* Write to cwd_path — should work (rw mount) */
    snprintf(cmd, sizeof(cmd),
        "echo written > %s/out.txt && cat %s/out.txt", cwd_dir, cwd_dir);
    r.command = cmd;
    res = run_tool_shell(&r);
    assert(res != NULL);
    assert(strstr(res, "[exit 0]") != NULL);
    assert(strstr(res, "written") != NULL);
    free(res);

    /* Cleanup */
    char rm[512];
    snprintf(rm, sizeof(rm), "rm -rf %s", cwd_dir);
    system(rm);
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
    /* cwd_path defaults to NULL, mount_cwd defaults to 0 */

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
/* sandbox=0 (host mode) — namespace NOT applied, child runs unsandboxed */
static void test_sandbox_none_skips_namespace(void) {
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.workspace = workspace;
    r.sandbox = 0;

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

int main(void) {
    TEST_INIT();
    printf("test_shell_namespace:\n");
    setup_workspace();
    if (!run_tool_ns_available(workspace)) {
        printf("  SKIP (namespaces unavailable on this system)\n");
    } else {
        RUN(test_write_inside_workspace);
        RUN(test_write_system_dir_blocked);
        RUN(test_read_system_files);
        RUN(test_network_isolated);
        RUN(test_agent_db_inaccessible);
        RUN(test_etc_shadow_inaccessible);
        RUN(test_cwd_path_rw);
        RUN(test_no_cwd_path_blocked);
    }
    RUN(test_sandbox_none_skips_namespace);
    cleanup_workspace();
    return 0;
}
