#define _GNU_SOURCE
#include "tool_shell.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Layer 2: Verify extra_mounts in SandboxConfig bind rw and ro paths correctly */

static char workspace[256];
static char rw_dir[256];
static char ro_dir[256];
static int ns_available = 1;

static void setup(void) {
    snprintf(workspace, sizeof(workspace), "/tmp/cclaw_mnt_test_%d_ws", getpid());
    snprintf(rw_dir, sizeof(rw_dir), "/tmp/cclaw_mnt_test_%d_rw", getpid());
    snprintf(ro_dir, sizeof(ro_dir), "/tmp/cclaw_mnt_test_%d_ro", getpid());
    mkdir(workspace, 0755);
    mkdir(rw_dir, 0755);
    mkdir(ro_dir, 0755);
    /* Seed ro_dir with a file so we can verify reads work */
    char path[512];
    snprintf(path, sizeof(path), "%s/readable.txt", ro_dir);
    FILE *f = fopen(path, "w");
    if (f) { fputs("readonly_content", f); fclose(f); }
}

static void cleanup(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf /tmp/cclaw_mnt_test_%d_*", getpid());
    system(cmd);
}

static void test_ns_available(void) {
    ShellConfig sc = {.timeout = 5, .workspace = workspace, .sandbox = 1};
    char *r = tool_shell_handler("{\"command\":\"echo ns_ok\"}", &sc);
    assert(r != NULL);
    if (strstr(r, "namespace sandbox unavailable")) {
        ns_available = 0;
        printf("  SKIP (namespaces unavailable)\n");
    } else {
        assert(strstr(r, "ns_ok") != NULL);
        printf("  PASS test_ns_available\n");
    }
    free(r);
}

/* Test: write to rw extra-mount succeeds */
static void test_write_rw_mount(void) {
    if (!ns_available) { printf("  SKIP test_write_rw_mount\n"); return; }
    char *rw_paths[] = { rw_dir };
    ShellConfig sc = {
        .timeout = 5, .workspace = workspace, .sandbox = 1,
        .write_paths = rw_paths, .write_path_count = 1
    };
    char args[1024];
    snprintf(args, sizeof(args),
        "{\"command\":\"echo hello > %s/written.txt && cat %s/written.txt\"}",
        rw_dir, rw_dir);
    char *r = tool_shell_handler(args, &sc);
    assert(r != NULL);
    assert(strstr(r, "hello") != NULL);
    assert(strstr(r, "[exit 0]") != NULL);
    free(r);
    printf("  PASS test_write_rw_mount\n");
}

/* Test: read from ro extra-mount succeeds */
static void test_read_ro_mount(void) {
    if (!ns_available) { printf("  SKIP test_read_ro_mount\n"); return; }
    char *ro_paths[] = { ro_dir };
    ShellConfig sc = {
        .timeout = 5, .workspace = workspace, .sandbox = 1,
        .read_paths = ro_paths, .read_path_count = 1
    };
    char args[512];
    snprintf(args, sizeof(args),
        "{\"command\":\"cat %s/readable.txt\"}", ro_dir);
    char *r = tool_shell_handler(args, &sc);
    assert(r != NULL);
    assert(strstr(r, "readonly_content") != NULL);
    free(r);
    printf("  PASS test_read_ro_mount\n");
}

/* Test: write to ro extra-mount fails */
static void test_write_ro_mount_fails(void) {
    if (!ns_available) { printf("  SKIP test_write_ro_mount_fails\n"); return; }
    char *ro_paths[] = { ro_dir };
    ShellConfig sc = {
        .timeout = 5, .workspace = workspace, .sandbox = 1,
        .read_paths = ro_paths, .read_path_count = 1
    };
    char args[512];
    snprintf(args, sizeof(args),
        "{\"command\":\"echo bad > %s/should_fail.txt 2>&1; echo exit=$?\"}", ro_dir);
    char *r = tool_shell_handler(args, &sc);
    assert(r != NULL);
    /* Write should fail — exit code non-zero or "Read-only" error */
    assert(strstr(r, "exit=0") == NULL || strstr(r, "Read-only") != NULL);
    free(r);
    printf("  PASS test_write_ro_mount_fails\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_sandbox_mounts\n");
    setup();
    test_ns_available();
    test_write_rw_mount();
    test_read_ro_mount();
    test_write_ro_mount_fails();
    cleanup();
    printf("All sandbox_mounts tests passed\n");
    return 0;
}
