/* T208/V85: Daemon namespace check — verify max_agents clamped by kernel limit */
#define _GNU_SOURCE
#include "daemon.h"
#include <assert.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

/* Test: daemon_get_max_agents returns a sane value (>0, ≤ DAEMON_MAX_CHILDREN) */
static void test_max_agents_sane(void) {
    int max = daemon_get_max_agents();
    assert(max > 0);
    assert(max <= DAEMON_MAX_CHILDREN);
    printf("  PASS test_max_agents_sane (max_agents=%d)\n", max);
}

/* Test: if namespaces available, max_agents ≤ max_user_namespaces/6 */
static void test_max_agents_respects_kernel(void) {
    FILE *f = fopen("/proc/sys/user/max_user_namespaces", "r");
    if (!f) {
        printf("  SKIP test_max_agents_respects_kernel (no procfs)\n");
        return;
    }
    int max_ns = 0;
    if (fscanf(f, "%d", &max_ns) != 1) max_ns = 0;
    fclose(f);

    if (max_ns <= 0) {
        printf("  SKIP test_max_agents_respects_kernel (max_ns=0)\n");
        return;
    }

    /* Test if unshare works */
    pid_t pid = fork();
    if (pid == 0) _exit(unshare(CLONE_NEWUSER) == 0 ? 0 : 1);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("  SKIP test_max_agents_respects_kernel (unshare failed)\n");
        return;
    }

    int max = daemon_get_max_agents();
    int ns_limit = max_ns / 6;
    /* If kernel constrains below DAEMON_MAX_CHILDREN, max should be clamped */
    if (ns_limit < DAEMON_MAX_CHILDREN) {
        assert(max <= ns_limit);
    }
    printf("  PASS test_max_agents_respects_kernel (max_ns=%d, limit=%d, max_agents=%d)\n",
           max_ns, ns_limit, max);
}

int main(void) {
    printf("test_namespace:\n");
    test_max_agents_sane();
    test_max_agents_respects_kernel();
    printf("All namespace tests passed.\n");
    return 0;
}
