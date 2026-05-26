/* T180: integration test — landlock blocks key file.
 * Fork child w/ landlock (workspace only), child attempts read of .cclaw_key,
 * verify EACCES; no network.
 * Cites V52, V22.
 *
 * Key insight: landlock_apply() adds /tmp as read-only, so we must place the
 * key file outside /tmp to test the block. We use CWD-relative paths. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include "landlock.h"
#include "secret.h"

static char basedir[256];
static char workspace[320];
static char key_path[320];
static char db_path[320];

static void setup(void) {
    /* Use a directory under CWD (not /tmp) so landlock read-only /tmp rule doesn't apply */
    char cwd[128];
    assert(getcwd(cwd, sizeof(cwd)) != NULL);
    snprintf(basedir, sizeof(basedir), "%s/build/t180_XXXXXX", cwd);
    assert(mkdtemp(basedir) != NULL);
    snprintf(workspace, sizeof(workspace), "%s/workspace", basedir);
    snprintf(key_path, sizeof(key_path), "%s/.cclaw_key", basedir);
    snprintf(db_path, sizeof(db_path), "%s/test.db", basedir);
    mkdir(workspace, 0700);

    /* Create key file outside workspace */
    uint8_t key[32];
    assert(secret_key_load_or_create(db_path, key) == 0);
}

static void teardown(void) {
    unlink(key_path);
    unlink(db_path);
    char ws_file[384];
    snprintf(ws_file, sizeof(ws_file), "%s/testfile", workspace);
    unlink(ws_file);
    rmdir(workspace);
    rmdir(basedir);
}

int main(void) {
    printf("test_integration_landlock_keyfile:\n");
    setup();

    /* Create a file inside workspace to verify workspace access works */
    char ws_file[384];
    snprintf(ws_file, sizeof(ws_file), "%s/testfile", workspace);
    FILE *f = fopen(ws_file, "w");
    assert(f);
    fprintf(f, "hello");
    fclose(f);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        /* Child: apply landlock with workspace only (no db_path, no session tmp) */
        int rc = landlock_apply(workspace, NULL, 0);
        if (rc < 0) {
            /* Landlock not available — exit with special code to skip test */
            _exit(77);
        }

        /* Verify workspace file IS readable */
        int fd = open(ws_file, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "FAIL: cannot read workspace file: %s\n", strerror(errno));
            _exit(1);
        }
        close(fd);

        /* Attempt to read key file — should be EACCES */
        fd = open(key_path, O_RDONLY);
        if (fd >= 0) {
            close(fd);
            fprintf(stderr, "FAIL: key file readable under landlock\n");
            _exit(2);
        }
        if (errno != EACCES) {
            fprintf(stderr, "FAIL: expected EACCES, got %s\n", strerror(errno));
            _exit(3);
        }

        /* Success */
        _exit(0);
    }

    /* Parent: wait for child */
    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 77) {
            printf("  landlock_blocks_keyfile... SKIP (landlock unavailable)\n");
            printf("  0/0 passed (skipped)\n");
        } else if (code == 0) {
            printf("  landlock_blocks_keyfile... PASS\n");
            printf("  1/1 passed\n");
        } else {
            printf("  landlock_blocks_keyfile... FAIL (exit %d)\n", code);
            teardown();
            return 1;
        }
    } else {
        printf("  landlock_blocks_keyfile... FAIL (signal %d)\n", WTERMSIG(status));
        teardown();
        return 1;
    }

    teardown();
    return 0;
}
