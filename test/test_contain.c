#define _GNU_SOURCE
#include "contain.h"
#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <stdint.h>
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

/* The Landlock half of the same question, asked of the kernel directly. */
static int really_landlocks(void) {
    return syscall(444 /* landlock_create_ruleset */, NULL, 0, 1u) >= 1;
}

/* The other direction: a kernel that has neither. A filter in a fresh fork
 * (nothing probed yet) answers landlock_create_ruleset with ENOSYS and every
 * way of installing a seccomp filter with EINVAL, as a kernel built without
 * them does; the probe must then report neither. */
static unsigned caps_on_kernel_without_them(void) {
    int pfd[2];
    assert(pipe(pfd) == 0);
    pid_t pid = fork();
    if (pid == 0) {
        /* Classic BPF by hand: no <linux/...> in the static builds. */
        struct insn { uint16_t code; uint8_t jt, jf; uint32_t k; } f[] = {
            { 0x20, 0, 0, 0 },                       /* ld nr */
            { 0x15, 0, 1, 444 },                     /* landlock_create_ruleset */
            { 0x06, 0, 0, 0x00050000u | ENOSYS },
            { 0x15, 0, 1, __NR_seccomp },
            { 0x06, 0, 0, 0x00050000u | EINVAL },
            { 0x15, 0, 3, __NR_prctl },
            { 0x20, 0, 0, 16 },                      /* ld args[0], low word */
            { 0x15, 0, 1, PR_SET_SECCOMP },
            { 0x06, 0, 0, 0x00050000u | EINVAL },
            { 0x06, 0, 0, 0x7fff0000u },             /* allow */
        };
        struct { unsigned short len; struct insn *f; } prog = { sizeof(f) / sizeof(f[0]), f };
        unsigned c = 0xffffffffu;
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 &&
            prctl(PR_SET_SECCOMP, 2 /* MODE_FILTER */, &prog, 0, 0) == 0)
            c = contain_caps(NULL, 0);
        _exit(write(pfd[1], &c, sizeof(c)) == sizeof(c) ? 0 : 1);
    }
    unsigned c = 0;
    assert(read(pfd[0], &c, sizeof(c)) == sizeof(c));
    waitpid(pid, NULL, 0);
    close(pfd[0]); close(pfd[1]);
    return c;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* First, before this process probes and caches. */
    unsigned without = caps_on_kernel_without_them();
    assert(without != 0xffffffffu);
    assert(!(without & (CONTAIN_CAP_LANDLOCK | CONTAIN_CAP_SECCOMP)));

    assert(contain_backend() && contain_backend()[0]);

    char why[160] = "stale";
    unsigned caps = contain_caps(why, sizeof(why));
    int ok = (caps & CONTAIN_CAP_MOUNTNS) != 0;
    if (strcmp(contain_backend(), "linux-ns") == 0)
        assert(ok == really_contains());
    else
        assert(caps == 0);
    assert(ok ? why[0] == '\0' : why[0] != '\0');   /* a reason only on failure */
    if (strcmp(contain_backend(), "linux-ns") == 0) {
        assert(!!(caps & CONTAIN_CAP_LANDLOCK) == really_landlocks());
        assert((caps & CONTAIN_CAP_LANDLOCK) ? contain_landlock_abi() >= 1
                                             : contain_landlock_abi() == 0);
    }
    printf("  backend=%s caps=%#x landlock_abi=%d%s%s\n", contain_backend(), caps,
           contain_landlock_abi(),
           ok ? "" : " why=", ok ? "" : why);

    /* Cached, and the caller's own process is never changed by the probe. */
    assert(contain_caps(NULL, 0) == caps);

    /* The test/diagnostic mask only ever takes capabilities away. */
    setenv("CCLAW_CONTAIN_CAPS_MASK", "0", 1);
    assert(contain_caps(why, sizeof(why)) == 0);
    assert(!ok || strstr(why, "CCLAW_CONTAIN_CAPS_MASK"));
    setenv("CCLAW_CONTAIN_CAPS_MASK", "0x17", 1);   /* everything but Landlock */
    assert(contain_caps(NULL, 0) == (caps & ~CONTAIN_CAP_LANDLOCK));
    setenv("CCLAW_CONTAIN_CAPS_MASK", "0xffffffff", 1);
    assert(contain_caps(NULL, 0) == caps);
    unsetenv("CCLAW_CONTAIN_CAPS_MASK");

    return 0;
}
