#define _GNU_SOURCE
#include "contain.h"
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

const char *contain_backend(void) { return "linux-ns"; }

/* The widest set the sandbox ever asks for (the shell tier; file/web/js omit
 * the PID namespace). Probing a narrower set would call a host usable that
 * then refuses the shell. */
#define CONTAIN_NS_FLAGS (CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWPID)

int contain_probe(char *why, size_t cap) {
    if (why && cap) why[0] = '\0';
    pid_t pid = fork();
    if (pid < 0) {
        if (why) snprintf(why, cap, "fork: %s", strerror(errno));
        return 0;
    }
    if (pid == 0) {
        /* errno rides home in the exit status; 0 is reserved for success. */
        if (unshare(CONTAIN_NS_FLAGS) == 0) _exit(0);
        _exit(errno > 0 && errno < 256 ? errno : 255);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno == EINTR) continue;
        if (why) snprintf(why, cap, "waitpid: %s", strerror(errno));
        return 0;
    }
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return 1;
    if (why) {
        if (WIFEXITED(st) && WEXITSTATUS(st) != 255)
            snprintf(why, cap, "unshare(user, mount, network, pid): %s",
                     strerror(WEXITSTATUS(st)));
        else
            snprintf(why, cap, "namespace probe child failed");
    }
    return 0;
}
