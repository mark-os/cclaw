#define _GNU_SOURCE
#include "contain.h"
#include <assert.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* What the probe claims must be what a child actually gets: ask for the same
 * namespaces the shell tier does and compare. A probe that says yes on a host
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
    int ok = contain_probe(why, sizeof(why));
    if (strcmp(contain_backend(), "linux-ns") == 0)
        assert(ok == really_contains());
    else
        assert(ok == 0);
    assert(ok ? why[0] == '\0' : why[0] != '\0');   /* a reason only on failure */
    printf("  backend=%s contained=%d%s%s\n", contain_backend(), ok,
           ok ? "" : " why=", ok ? "" : why);

    /* It runs in a throwaway child, so asking twice gives the same answer. */
    assert(contain_probe(NULL, 0) == ok);

    return 0;
}
