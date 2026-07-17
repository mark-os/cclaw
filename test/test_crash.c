/* Test: crash handler emits the fatal signal line and preserves WTERMSIG */
#define _POSIX_C_SOURCE 200809L
#include "crash.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_util.h"

/* Fork a child that installs the crash handler then triggers a fatal signal.
 * Parent captures stderr via pipe and checks for the expected output. */
static void test_signal(int sig, const char *expected_name) {
    int pipefd[2];
    assert(pipe(pipefd) == 0);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        /* Child: redirect stderr to pipe write end */
        close(pipefd[0]);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        crash_handler_init();

        /* Trigger the signal */
        if (sig == SIGSEGV) {
            volatile int *p = NULL;
            *p = 42;  /* dereference NULL */
        } else {
            raise(sig);
        }
        _exit(99);  /* should not reach here */
    }

    /* Parent: read child's stderr */
    close(pipefd[1]);

    char buf[4096];
    size_t total = 0;
    ssize_t n;
    while ((n = read(pipefd[0], buf + total, sizeof(buf) - total - 1)) > 0)
        total += (size_t)n;
    buf[total] = '\0';
    close(pipefd[0]);

    /* Wait for child */
    int wstatus;
    assert(waitpid(pid, &wstatus, 0) == pid);

    /* Child must have died by signal */
    assert(WIFSIGNALED(wstatus));
    assert(WTERMSIG(wstatus) == sig);

    /* Check for the expected "fatal signal=..." line */
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "fatal signal=%s", expected_name);
    assert(strstr(buf, pattern) != NULL);

    /* Check pid= is present */
    assert(strstr(buf, "pid=") != NULL);

#if defined(__GLIBC__)
    /* Under glibc, expect at least one backtrace frame (contains "0x" or "[") */
    assert(strstr(buf, "0x") != NULL || strstr(buf, "[") != NULL);
#endif

    printf("  signal=%s... PASS\n", expected_name);
}

int main(void) {
    TEST_INIT();
    printf("test_crash:\n");

    test_signal(SIGSEGV, "SIGSEGV");
    test_signal(SIGABRT, "SIGABRT");
    test_signal(SIGBUS,  "SIGBUS");
    test_signal(SIGFPE,  "SIGFPE");

    printf("all crash tests passed\n");
    return 0;
}
