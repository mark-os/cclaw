#define _GNU_SOURCE
#include "crash.h"
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>

#if defined(__GLIBC__)
#include <execinfo.h>
#define HAVE_BACKTRACE 1
#else
#define HAVE_BACKTRACE 0
#endif

/* Async-signal-safe integer to decimal string. Writes into buf (must hold ≥21
 * bytes for int64 max). Returns pointer to first digit within buf. */
static char *safe_itoa(char *buf, size_t cap, long val) {
    int neg = 0;
    if (val < 0) { neg = 1; val = -val; }
    char *end = buf + cap - 1;
    *end = '\0';
    char *p = end;
    if (val == 0) { *(--p) = '0'; }
    else { while (val > 0) { *(--p) = '0' + (char)(val % 10); val /= 10; } }
    if (neg) *(--p) = '-';
    return p;
}

/* Signal number → short name (async-signal-safe, no allocations) */
static const char *sig_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGBUS:  return "SIGBUS";
        case SIGILL:  return "SIGILL";
        case SIGFPE:  return "SIGFPE";
        default:      return "UNKNOWN";
    }
}

/* The handler itself — async-signal-safe only. Writes logfmt-style line to
 * stderr (fd 2), optionally a backtrace, then re-raises for default action. */
static void crash_handler(int sig) {
    /* Build: "fatal signal=SIGSEGV pid=12345\n" using only write() */
    char numbuf[24];
    const char *name = sig_name(sig);
    char *pidstr = safe_itoa(numbuf, sizeof(numbuf), (long)getpid());

    /* Use writev-style writes (multiple write() calls are fine on fd 2) */
    (void)write(STDERR_FILENO, "fatal signal=", 13);
    (void)write(STDERR_FILENO, name, strlen(name));
    (void)write(STDERR_FILENO, " pid=", 5);
    (void)write(STDERR_FILENO, pidstr, strlen(pidstr));
    (void)write(STDERR_FILENO, "\n", 1);

#if HAVE_BACKTRACE
    void *frames[64];
    int n = backtrace(frames, 64);
    if (n > 0)
        backtrace_symbols_fd(frames, n, STDERR_FILENO);
#endif

    /* Re-raise with default disposition (SA_RESETHAND already restored it) */
    raise(sig);
}

/* Alternate signal stack so SIGSEGV from stack overflow can still run.
 * SIGSTKSZ may not be a compile-time constant on newer glibc, so use a
 * fixed size that's generous enough for the handler + backtrace. */
#define CRASH_ALT_STACK_SIZE 16384
static char alt_stack_mem[CRASH_ALT_STACK_SIZE];

void crash_handler_init(void) {
    /* Set up alternate stack */
    stack_t ss = {
        .ss_sp = alt_stack_mem,
        .ss_size = sizeof(alt_stack_mem),
        .ss_flags = 0
    };
    sigaltstack(&ss, NULL);

#if HAVE_BACKTRACE
    /* Force libgcc's lazy first-call malloc outside the handler. backtrace()
     * on glibc's first invocation may call dl_iterate_phdr → malloc; do it
     * now so the signal handler path is allocation-free. */
    void *dummy[1];
    (void)backtrace(dummy, 1);
#endif

    /* Install handler for fatal signals */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND | SA_NODEFER | SA_ONSTACK;

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
}
