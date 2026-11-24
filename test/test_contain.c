#define _GNU_SOURCE
#include "contain.h"
#include <assert.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* What the probe claims must be what a child actually gets: ask for the same
 * namespaces the shell does and compare. A probe that says yes on a host
 * that then refuses the shell (or no on one that would run it) is worse than
 * no probe — doctor and first-run both act on its answer. */
static int really_contains(void) {
    pid_t pid = fork();
    if (pid == 0)
        _exit(unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWPID) == 0 ? 0 : 1);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);

    assert(contain_backend() && contain_backend()[0]);

    char why[160] = "stale";
    unsigned caps = contain_caps(why, sizeof(why));
    int ok = (caps & CONTAIN_CAP_MOUNTNS) != 0;
    if (strcmp(contain_backend(), "linux-ns") == 0)
        assert(ok == really_contains());
    else
        assert(caps == 0);
    assert(ok ? why[0] == '\0' : why[0] != '\0');   /* a reason only on failure */
    printf("  backend=%s caps=%#x%s%s\n", contain_backend(), caps,
           ok ? "" : " why=", ok ? "" : why);

    /* Cached, and the caller's own process is never changed by the probe. */
    assert(contain_caps(NULL, 0) == caps);

    /* The test/diagnostic mask only ever takes capabilities away. */
    setenv("CCLAW_CONTAIN_CAPS_MASK", "0", 1);
    assert(contain_caps(why, sizeof(why)) == 0);
    assert(!ok || strstr(why, "CCLAW_CONTAIN_CAPS_MASK"));
    setenv("CCLAW_CONTAIN_CAPS_MASK", "0xffffffff", 1);
    assert(contain_caps(NULL, 0) == caps);
    unsetenv("CCLAW_CONTAIN_CAPS_MASK");

    return 0;
}
