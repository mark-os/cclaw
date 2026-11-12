#define _GNU_SOURCE
#include "contain.h"
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;
#define ASSERT(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while(0)

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

    ASSERT(contain_backend() && contain_backend()[0], "backend has a name");

    char why[160] = "stale";
    int ok = contain_probe(why, sizeof(why));
    ASSERT(ok == really_contains(), "probe agrees with a real unshare");
    if (ok) ASSERT(why[0] == '\0', "no reason given on success");
    else    ASSERT(why[0] != '\0', "a reason is given on failure");
    printf("  backend=%s contained=%d%s%s\n", contain_backend(), ok,
           ok ? "" : " why=", ok ? "" : why);

    /* It runs in a throwaway child, so asking twice gives the same answer. */
    ASSERT(contain_probe(NULL, 0) == ok, "probe is repeatable and takes no buffer");

    if (failures) { printf("FAIL: test_contain (%d failures)\n", failures); return 1; }
    printf("PASS: test_contain\n");
    return 0;
}
