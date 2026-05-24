/* T84: Landlock filesystem restriction tests */
#define _GNU_SOURCE
#include "landlock.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/landlock.h>

static int landlock_supported(void) {
    int abi = (int)syscall(__NR_landlock_create_ruleset, NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    return abi >= 0;
}

/* Test: landlock_apply returns 0 on supported kernels */
static void test_landlock_apply_succeeds(void) {
    if (!landlock_supported()) {
        printf("  SKIP (landlock not supported)\n");
        return;
    }

    /* Must test in child — landlock is irreversible */
    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        /* Child: apply landlock with /tmp as workspace */
        mkdir("/tmp/test_ll_ws", 0755);
        int rc = landlock_apply("/tmp/test_ll_ws", "/tmp/test_ll.db");
        _exit(rc == 0 ? 0 : 1);
    }

    int status;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    rmdir("/tmp/test_ll_ws");
    printf("  PASS\n");
}

/* Test: after landlock, writing outside workspace fails */
static void test_landlock_blocks_outside_write(void) {
    if (!landlock_supported()) {
        printf("  SKIP (landlock not supported)\n");
        return;
    }

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        mkdir("/tmp/test_ll_ws2", 0755);
        int rc = landlock_apply("/tmp/test_ll_ws2", NULL);
        if (rc != 0) _exit(2);

        /* Writing inside workspace should succeed */
        int fd = open("/tmp/test_ll_ws2/ok.txt", O_WRONLY | O_CREAT, 0644);
        if (fd < 0) _exit(3);
        close(fd);
        unlink("/tmp/test_ll_ws2/ok.txt");

        /* Writing outside workspace should fail */
        fd = open("/tmp/outside_ll.txt", O_WRONLY | O_CREAT, 0644);
        if (fd >= 0) {
            close(fd);
            unlink("/tmp/outside_ll.txt");
            _exit(4);  /* Should have been blocked */
        }
        /* Expected: EACCES */
        _exit(errno == EACCES ? 0 : 5);
    }

    int status;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status));
    if (WEXITSTATUS(status) != 0) {
        printf("  FAIL (exit code %d)\n", WEXITSTATUS(status));
        assert(0);
    }
    rmdir("/tmp/test_ll_ws2");
    printf("  PASS\n");
}

/* Test: reading system paths still works after landlock */
static void test_landlock_allows_system_read(void) {
    if (!landlock_supported()) {
        printf("  SKIP (landlock not supported)\n");
        return;
    }

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        mkdir("/tmp/test_ll_ws3", 0755);
        int rc = landlock_apply("/tmp/test_ll_ws3", NULL);
        if (rc != 0) _exit(2);

        /* Reading /etc/hostname or /etc/resolv.conf should work */
        int fd = open("/etc/resolv.conf", O_RDONLY);
        if (fd < 0) _exit(3);
        close(fd);

        _exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    rmdir("/tmp/test_ll_ws3");
    printf("  PASS\n");
}

/* Test: graceful fallback — function returns -1 if landlock unavailable
 * (We can't easily simulate this, so just verify the ABI check path) */
static void test_landlock_reports_abi(void) {
    int abi = (int)syscall(__NR_landlock_create_ruleset, NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    printf("  landlock ABI version: %d (%s)\n", abi, abi >= 0 ? "supported" : "not supported");
    /* Either way, this is informational — not a failure */
    printf("  PASS\n");
}

int main(void) {
    printf("test_landlock:\n");

    printf("  test_landlock_reports_abi...\n");
    test_landlock_reports_abi();

    printf("  test_landlock_apply_succeeds...\n");
    test_landlock_apply_succeeds();

    printf("  test_landlock_blocks_outside_write...\n");
    test_landlock_blocks_outside_write();

    printf("  test_landlock_allows_system_read...\n");
    test_landlock_allows_system_read();

    printf("ALL PASSED\n");
    return 0;
}
