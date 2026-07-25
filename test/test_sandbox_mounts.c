#define _GNU_SOURCE
#include "test_run_tool_shell.h"
#include <sys/stat.h>
#include "test_util.h"

/* Layer 2: Verify extra mounts bind rw and ro paths correctly via --run-tool */

static char workspace[256];
static char rw_dir[256];
static char ro_dir[256];
static int ns_available_flag = 1;

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
    /* Sibling in the same directory: a file grant on readable.txt must not
     * bring this along, or the grant is really a directory grant. */
    snprintf(path, sizeof(path), "%s/sibling.txt", ro_dir);
    f = fopen(path, "w");
    if (f) { fputs("sibling_content", f); fclose(f); }
}

static void cleanup(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf /tmp/cclaw_mnt_test_%d_*", getpid());
    system(cmd);
}

static void test_ns_available(void) {
    ns_available_flag = run_tool_ns_available(workspace);
    if (!ns_available_flag)
        printf("  SKIP (namespaces unavailable)\n");
    else
        printf("  PASS test_ns_available\n");
}

/* Test: write to rw extra-mount succeeds */
static void test_write_rw_mount(void) {
    if (!ns_available_flag) { printf("  SKIP test_write_rw_mount\n"); return; }
    const char *wp[] = { rw_dir };
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "echo hello > %s/written.txt && cat %s/written.txt", rw_dir, rw_dir);

    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = cmd;
    r.workspace = workspace;
    r.write_paths = wp;
    r.write_count = 1;

    char *res = run_tool_shell(&r);
    assert(res != NULL);
    assert(strstr(res, "hello") != NULL);
    assert(strstr(res, "[exit 0]") != NULL);
    free(res);
    printf("  PASS test_write_rw_mount\n");
}

/* Test: read from ro extra-mount succeeds */
static void test_read_ro_mount(void) {
    if (!ns_available_flag) { printf("  SKIP test_read_ro_mount\n"); return; }
    const char *rp[] = { ro_dir };
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "cat %s/readable.txt", ro_dir);

    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = cmd;
    r.workspace = workspace;
    r.read_paths = rp;
    r.read_count = 1;

    char *res = run_tool_shell(&r);
    assert(res != NULL);
    assert(strstr(res, "readonly_content") != NULL);
    free(res);
    printf("  PASS test_read_ro_mount\n");
}

/* Test: write to ro extra-mount fails */
static void test_write_ro_mount_fails(void) {
    if (!ns_available_flag) { printf("  SKIP test_write_ro_mount_fails\n"); return; }
    const char *rp[] = { ro_dir };
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "echo bad > %s/should_fail.txt 2>&1; echo exit=$?", ro_dir);

    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = cmd;
    r.workspace = workspace;
    r.read_paths = rp;
    r.read_count = 1;

    char *res = run_tool_shell(&r);
    assert(res != NULL);
    /* Write should fail — exit code non-zero or "Read-only" error */
    assert(strstr(res, "exit=0") == NULL || strstr(res, "Read-only") != NULL);
    free(res);
    printf("  PASS test_write_ro_mount_fails\n");
}

/* A grant naming a single file mounts that file and nothing else. This is what
 * makes "request the narrowest grant that unblocks the task" followable — the
 * mount layer used to round every grant up to a directory (bind_path_into
 * refused non-directories), so an agent needing one file had to be handed its
 * whole parent. */
static void test_file_grant_mounts_only_that_file(void) {
    if (!ns_available_flag) { printf("  SKIP test_file_grant_mounts_only_that_file\n"); return; }
    char granted[512];
    snprintf(granted, sizeof(granted), "%s/readable.txt", ro_dir);
    const char *rp[] = { granted };
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "cat %s 2>&1; cat %s/sibling.txt 2>&1; "
        "echo tampered > %s 2>&1 && echo WROTE_RO", granted, ro_dir, granted);

    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = cmd;
    r.workspace = workspace;
    r.read_paths = rp;
    r.read_count = 1;

    char *res = run_tool_shell(&r);
    assert(res != NULL);
    assert(strstr(res, "readonly_content") != NULL);
    assert(strstr(res, "sibling_content") == NULL);
    /* A read grant on a file must be read-only: the ro remount has to take on
     * a file bind, not just a directory one. */
    assert(strstr(res, "WROTE_RO") == NULL);
    free(res);
    printf("  PASS test_file_grant_mounts_only_that_file\n");
}

/* A write grant on a single file is writable, and the write lands on the host
 * (the bind is the real file, not a copy). */
static void test_file_write_grant_persists(void) {
    if (!ns_available_flag) { printf("  SKIP test_file_write_grant_persists\n"); return; }
    char target[512];
    snprintf(target, sizeof(target), "%s/writable.txt", rw_dir);
    FILE *f = fopen(target, "w");
    if (f) { fputs("before", f); fclose(f); }

    const char *wp[] = { target };
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "echo after > %s; echo exit=$?", target);

    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = cmd;
    r.workspace = workspace;
    r.write_paths = wp;
    r.write_count = 1;

    char *res = run_tool_shell(&r);
    assert(res != NULL);
    assert(strstr(res, "exit=0") != NULL);
    free(res);

    char buf[64] = {0};
    f = fopen(target, "r");
    assert(f);
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    assert(strstr(buf, "after") != NULL);
    printf("  PASS test_file_write_grant_persists\n");
}

int main(void) {
    TEST_INIT();
    printf("test_sandbox_mounts\n");
    setup();
    test_ns_available();
    test_write_rw_mount();
    test_read_ro_mount();
    test_write_ro_mount_fails();
    test_file_grant_mounts_only_that_file();
    test_file_write_grant_persists();
    cleanup();
    printf("All sandbox_mounts tests passed\n");
    return 0;
}
