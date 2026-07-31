#define _GNU_SOURCE
#include "test_run_tool_shell.h"
#include <limits.h>
#include <sys/stat.h>
#include "test_util.h"

/* Layer 2: Verify extra mounts bind rw and ro paths correctly via --run-tool,
 * and that what the child is *told* matches what was actually mounted (HOME,
 * grant-mount failures). */

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

/* Read the first bytes of a host file into buf (NUL-terminated). */
static void read_host_file(const char *path, char *buf, size_t sz) {
    buf[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    size_t n = fread(buf, 1, sz - 1, f);
    fclose(f);
    buf[n] = '\0';
}

/* A file grant whose mount point falls inside an already-mounted read-only
 * directory grant cannot be created (EROFS), so the grant does not mount. That
 * is fail-open by design — a grant that fails to mount only ever narrows what
 * the child can reach — but it must not be *silent*: an operator approved it
 * and the agent was told it has it. Assert the child says so out loud.
 *
 * Positive control in the same function: the identical file grant, with the
 * enclosing read-only grant removed, mounts and writes through to the host —
 * so a vacuous "nothing worked" run cannot pass this test. */
static void test_nested_file_grant_is_loud(void) {
    if (!ns_available_flag) { printf("  SKIP test_nested_file_grant_is_loud\n"); return; }
    char nested[512];
    snprintf(nested, sizeof(nested), "%s/nested.txt", ro_dir);

    FILE *f = fopen(nested, "w");
    assert(f); fputs("before", f); fclose(f);

    const char *rp[] = { ro_dir };
    const char *wp[] = { nested };
    char cmd[1024];
    /* Subshell: a redirection failure cannot take the outer shell down, so
     * "exit=" always prints and a missing marker means a broken run, not a
     * failed write. */
    snprintf(cmd, sizeof(cmd), "( echo after > %s ) 2>&1; echo exit=$?", nested);

    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = cmd;
    r.workspace = workspace;
    r.read_paths = rp;
    r.read_count = 1;
    r.write_paths = wp;
    r.write_count = 1;

    char *res = run_tool_shell(&r);
    assert(res != NULL);
    /* The honest report: names the grant, names the read-only mount that
     * swallowed it, and says the grant is not in effect. */
    char expect_ro[640];
    snprintf(expect_ro, sizeof(expect_ro), "read-only mount '%s'", ro_dir);
    assert(strstr(res, "is inside read-only mount") != NULL);
    assert(strstr(res, nested) != NULL);
    assert(strstr(res, expect_ro) != NULL);   /* names the right mount, not /tmp */
    assert(strstr(res, "NOT in effect") != NULL);
    /* Fail-open, not fail-closed: the child ran (the shell produced output),
     * it just did not get that grant. */
    assert(strstr(res, "exit=") != NULL);
    assert(strstr(res, "exit=0") == NULL);
    free(res);

    char buf[64];
    read_host_file(nested, buf, sizeof(buf));
    assert(strcmp(buf, "before") == 0);

    /* Positive control: same file grant, no enclosing read-only grant. */
    ShellToolReq ctl = SHELL_REQ_DEFAULTS;
    ctl.command = cmd;
    ctl.workspace = workspace;
    ctl.write_paths = wp;
    ctl.write_count = 1;

    res = run_tool_shell(&ctl);
    assert(res != NULL);
    assert(strstr(res, "exit=0") != NULL);
    assert(strstr(res, "is inside read-only mount") == NULL);
    assert(strstr(res, "NOT in effect") == NULL);
    free(res);

    read_host_file(nested, buf, sizeof(buf));
    assert(strstr(buf, "after") != NULL);
    printf("  PASS test_nested_file_grant_is_loud\n");
}

/* HOME must be the path the workspace was actually mounted at. The bind mount
 * and the post-pivot chdir both use realpath(workspace), so a HOME built from
 * the raw path names a directory that does not exist inside the namespace when
 * the configured path has a symlink component — every toolchain that writes
 * under $HOME then fails on a workspace that is right there.
 *
 * Positive control in the same function: the same workspace named directly
 * (no symlink) still yields a HOME that exists and is the workspace. */
static void test_home_follows_resolved_workspace(void) {
    if (!ns_available_flag) { printf("  SKIP test_home_follows_resolved_workspace\n"); return; }
    char real_ws[256], link_ws[256], resolved[PATH_MAX], expect[PATH_MAX + 16];
    snprintf(real_ws, sizeof(real_ws), "/tmp/cclaw_mnt_test_%d_hreal", getpid());
    snprintf(link_ws, sizeof(link_ws), "/tmp/cclaw_mnt_test_%d_hlink", getpid());
    mkdir(real_ws, 0755);
    unlink(link_ws);
    assert(symlink(real_ws, link_ws) == 0);
    assert(realpath(link_ws, resolved) != NULL);

    /* Brackets make the match exact: the buggy HOME (the symlink path) has the
     * resolved path as a prefix, so a bare substring test would pass on it. */
    const char *cmd =
        "printf 'HOME=[%s]\\n' \"$HOME\"; "
        "test -d \"$HOME\" && echo HOME_IS_DIR || echo HOME_MISSING";
    snprintf(expect, sizeof(expect), "HOME=[%s]", resolved);

    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = cmd;
    r.workspace = link_ws;
    char *res = run_tool_shell(&r);
    assert(res != NULL);
    assert(strstr(res, expect) != NULL);
    assert(strstr(res, "HOME_IS_DIR") != NULL);
    assert(strstr(res, "HOME_MISSING") == NULL);
    free(res);

    /* Positive control: the unsymlinked workspace behaves the same. */
    r.workspace = real_ws;
    res = run_tool_shell(&r);
    assert(res != NULL);
    assert(strstr(res, expect) != NULL);
    assert(strstr(res, "HOME_IS_DIR") != NULL);
    assert(strstr(res, "HOME_MISSING") == NULL);
    free(res);
    printf("  PASS test_home_follows_resolved_workspace\n");
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
    test_nested_file_grant_is_loud();
    test_home_follows_resolved_workspace();
    cleanup();
    printf("All sandbox_mounts tests passed\n");
    return 0;
}
