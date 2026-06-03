#define _GNU_SOURCE
#include "tool_shell.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* T209: Verify namespace sandbox restricts filesystem writes outside workspace */

static char workspace[256];
static int ns_available = 1;

static void setup_workspace(void) {
    snprintf(workspace, sizeof(workspace), "/tmp/cclaw_ns_test_%d", getpid());
    mkdir(workspace, 0755);
}

static void cleanup_workspace(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", workspace);
    system(cmd);
}

/* Detect if namespaces work by checking first command output */
static void test_namespace_active(void) {
    ShellConfig sc = {.timeout = 5, .workspace = workspace};
    char *r = tool_shell_handler("{\"command\":\"echo ns_check\"}", &sc);
    assert(r != NULL);
    if (strstr(r, "namespace sandbox unavailable")) {
        ns_available = 0;
        printf("  SKIP (namespaces unavailable on this system)\n");
    } else {
        assert(strstr(r, "ns_check") != NULL);
        printf("  PASS test_namespace_active\n");
    }
    free(r);
}

/* Test: writing inside workspace succeeds */
static void test_write_inside_workspace(void) {
    if (!ns_available) { printf("  SKIP test_write_inside_workspace\n"); return; }
    ShellConfig sc = {.timeout = 5, .workspace = workspace};
    char args[1024];
    snprintf(args, sizeof(args),
        "{\"command\":\"echo hello > %s/test_file.txt && cat %s/test_file.txt\"}",
        workspace, workspace);
    char *r = tool_shell_handler(args, &sc);
    assert(r != NULL);
    assert(strstr(r, "[exit 0]") != NULL);
    assert(strstr(r, "hello") != NULL);
    free(r);
    printf("  PASS test_write_inside_workspace\n");
}

/* Test: writing to /etc fails (read-only bind mount) */
static void test_write_system_dir_blocked(void) {
    if (!ns_available) { printf("  SKIP test_write_system_dir_blocked\n"); return; }
    ShellConfig sc = {.timeout = 5, .workspace = workspace};
    char *r = tool_shell_handler(
        "{\"command\":\"touch /etc/cclaw_escape_test 2>&1; echo rc=$?\"}", &sc);
    assert(r != NULL);
    /* /etc is ro — touch should fail */
    assert(strstr(r, "rc=1") != NULL || strstr(r, "Read-only") != NULL);
    free(r);
    printf("  PASS test_write_system_dir_blocked\n");
}

/* Test: reading system files still works (/ is ro, not invisible) */
static void test_read_system_files(void) {
    if (!ns_available) { printf("  SKIP test_read_system_files\n"); return; }
    ShellConfig sc = {.timeout = 5, .workspace = workspace};
    char *r = tool_shell_handler("{\"command\":\"ls /bin/sh\"}", &sc);
    assert(r != NULL);
    assert(strstr(r, "[exit 0]") != NULL);
    assert(strstr(r, "/bin/sh") != NULL);
    free(r);
    printf("  PASS test_read_system_files\n");
}

/* Test: network is isolated (CLONE_NEWNET = no interfaces except lo down) */
static void test_network_isolated(void) {
    if (!ns_available) { printf("  SKIP test_network_isolated\n"); return; }
    ShellConfig sc = {.timeout = 5, .workspace = workspace};
    /* In a new network namespace, only lo exists and it's down.
     * Some kernels also create sit0 (IPv6-over-IPv4 tunnel) in new netns. */
    char *r = tool_shell_handler(
        "{\"command\":\"cat /proc/net/dev | grep ':' | grep -v 'lo' | grep -v 'sit0' | wc -l\"}", &sc);
    assert(r != NULL);
    /* Should have 0 non-lo, non-sit0 interfaces */
    if (strstr(r, "[exit 0]")) {
        assert(strstr(r, "\n0\n") != NULL);
    }
    free(r);
    printf("  PASS test_network_isolated\n");
}

/* Test: graceful fallback — handler still works even if namespace fails */
static void test_graceful_fallback(void) {
    /* Without workspace, namespace still attempted; command should work either way */
    char *r = tool_shell_handler("{\"command\":\"echo works\"}", NULL);
    assert(r != NULL);
    assert(strstr(r, "works") != NULL);
    free(r);
    printf("  PASS test_graceful_fallback\n");
}

/* Test: agent DB path is not accessible from shell child */
static void test_agent_db_inaccessible(void) {
    if (!ns_available) { printf("  SKIP test_agent_db_inaccessible\n"); return; }
    ShellConfig sc = {.timeout = 5, .workspace = workspace};
    /* Try to read a path outside the sandbox — should not exist in new root */
    char *r = tool_shell_handler(
        "{\"command\":\"cat /root/.bashrc 2>&1; echo rc=$?\"}", &sc);
    assert(r != NULL);
    /* Path shouldn't exist in sandboxed filesystem */
    assert(strstr(r, "rc=1") != NULL || strstr(r, "No such file") != NULL);
    free(r);
    printf("  PASS test_agent_db_inaccessible\n");
}

/* T217: Verify /etc/shadow inaccessible from shell child */
static void test_etc_shadow_inaccessible(void) {
    if (!ns_available) { printf("  SKIP test_etc_shadow_inaccessible\n"); return; }
    ShellConfig sc = {.timeout = 5, .workspace = workspace};
    char *r = tool_shell_handler(
        "{\"command\":\"cat /etc/shadow 2>&1; echo rc=$?\"}", &sc);
    assert(r != NULL);
    /* /etc is ro bind-mount; shadow should be permission-denied or not exist */
    assert(strstr(r, "rc=1") != NULL || strstr(r, "Permission denied") != NULL
           || strstr(r, "No such file") != NULL);
    free(r);
    printf("  PASS test_etc_shadow_inaccessible\n");
}

/* T217: Verify agent.db path inaccessible from shell child */
static void test_agent_db_path_blocked(void) {
    if (!ns_available) { printf("  SKIP test_agent_db_path_blocked\n"); return; }
    ShellConfig sc = {.timeout = 5, .workspace = workspace};
    /* Typical agent.db path — outside workspace, not bind-mounted */
    char *r = tool_shell_handler(
        "{\"command\":\"cat .cclaw/agents/default/agent.db 2>&1; echo rc=$?\"}", &sc);
    assert(r != NULL);
    assert(strstr(r, "rc=1") != NULL || strstr(r, "No such file") != NULL);
    free(r);
    printf("  PASS test_agent_db_path_blocked\n");
}

/* T217: Verify cclaw.db path inaccessible from shell child */
static void test_daemon_db_path_blocked(void) {
    if (!ns_available) { printf("  SKIP test_daemon_db_path_blocked\n"); return; }
    ShellConfig sc = {.timeout = 5, .workspace = workspace};
    char *r = tool_shell_handler(
        "{\"command\":\"cat .cclaw/cclaw.db 2>&1; echo rc=$?\"}", &sc);
    assert(r != NULL);
    assert(strstr(r, "rc=1") != NULL || strstr(r, "No such file") != NULL);
    free(r);
    printf("  PASS test_daemon_db_path_blocked\n");
}

/* T276/V22a: CWD rw bind-mount in CLI mode — cwd_path accessible rw alongside workspace */
static void test_cwd_path_rw(void) {
    if (!ns_available) { printf("  SKIP test_cwd_path_rw\n"); return; }

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

    ShellConfig sc = {.timeout = 5, .workspace = workspace, .cwd_path = cwd_dir};
    char args[1024];

    /* Read from cwd_path — should work */
    snprintf(args, sizeof(args), "{\"command\":\"cat %s/seed.txt\"}", cwd_dir);
    char *r = tool_shell_handler(args, &sc);
    assert(r != NULL);
    assert(strstr(r, "[exit 0]") != NULL);
    assert(strstr(r, "cwd_content") != NULL);
    free(r);

    /* Write to cwd_path — should work (rw mount) */
    snprintf(args, sizeof(args),
        "{\"command\":\"echo written > %s/out.txt && cat %s/out.txt\"}", cwd_dir, cwd_dir);
    r = tool_shell_handler(args, &sc);
    assert(r != NULL);
    assert(strstr(r, "[exit 0]") != NULL);
    assert(strstr(r, "written") != NULL);
    free(r);

    /* Cleanup */
    snprintf(args, sizeof(args), "rm -rf %s", cwd_dir);
    system(args);
    printf("  PASS test_cwd_path_rw\n");
}

/* T276: Without cwd_path (daemon mode), CWD dir is NOT accessible */
static void test_no_cwd_path_blocked(void) {
    if (!ns_available) { printf("  SKIP test_no_cwd_path_blocked\n"); return; }

    char cwd_dir[256];
    snprintf(cwd_dir, sizeof(cwd_dir), "/tmp/cclaw_nocwd_test_%d", getpid());
    mkdir(cwd_dir, 0755);
    char seed[512];
    snprintf(seed, sizeof(seed), "%s/seed.txt", cwd_dir);
    FILE *f = fopen(seed, "w");
    fprintf(f, "secret\n");
    fclose(f);

    /* No cwd_path — simulates daemon mode */
    ShellConfig sc = {.timeout = 5, .workspace = workspace, .cwd_path = NULL};
    char args[1024];
    snprintf(args, sizeof(args), "{\"command\":\"cat %s/seed.txt 2>&1; echo rc=$?\"}", cwd_dir);
    char *r = tool_shell_handler(args, &sc);
    assert(r != NULL);
    assert(strstr(r, "rc=1") != NULL || strstr(r, "No such file") != NULL);
    free(r);

    snprintf(args, sizeof(args), "rm -rf %s", cwd_dir);
    system(args);
    printf("  PASS test_no_cwd_path_blocked\n");
}

int main(void) {
    printf("test_shell_namespace (T209/T217/T276):\n");
    setup_workspace();
    test_namespace_active();
    test_write_inside_workspace();
    test_write_system_dir_blocked();
    test_read_system_files();
    test_network_isolated();
    test_graceful_fallback();
    test_agent_db_inaccessible();
    test_etc_shadow_inaccessible();
    test_agent_db_path_blocked();
    test_daemon_db_path_blocked();
    test_cwd_path_rw();
    test_no_cwd_path_blocked();
    cleanup_workspace();
    printf("All namespace sandbox tests passed.\n");
    return 0;
}
