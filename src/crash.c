#define _GNU_SOURCE
#include "crash.h"
#include <signal.h>
#include <syslog.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>

#if defined(__linux__)
#include <ucontext.h>
#endif

#if defined(__GLIBC__)
#include <execinfo.h>
#define HAVE_BACKTRACE 1
#else
#define HAVE_BACKTRACE 0
#endif

/* Async-signal-safe hex formatter. Writes "0x..." into buf (must hold ≥19 bytes).
 * Returns pointer within buf. */
static char *safe_hex(char *buf, size_t cap, unsigned long val) {
    char *end = buf + cap - 1;
    *end = '\0';
    char *p = end;
    if (val == 0) { *(--p) = '0'; }
    else {
        while (val > 0 && p > buf + 2) {
            int d = (int)(val & 0xf);
            *(--p) = (char)(d < 10 ? '0' + d : 'a' + d - 10);
            val >>= 4;
        }
    }
    *(--p) = 'x'; *(--p) = '0';
    return p;
}

/* Async-signal-safe integer to decimal string. */
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

/* The handler itself — async-signal-safe only. Writes logfmt-style crash info
 * to stderr AND syslog, including the PC/LR registers for addr2line on the dev
 * machine. Then re-raises for default action (core dump). */
static void crash_handler(int sig, siginfo_t *info, void *uctx) {
    char buf[256];
    int n = 0;
    char numbuf[24];
    const char *name = sig_name(sig);
    char *pidstr = safe_itoa(numbuf, sizeof(numbuf), (long)getpid());

    n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                  "fatal signal=%s pid=%s", name, pidstr);

    if (info)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                      " addr=%p", info->si_addr);

#if defined(__linux__)
    ucontext_t *uc = (ucontext_t *)uctx;
    if (uc) {
        char hexbuf[24];
#if defined(__aarch64__)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                      " pc=%s", safe_hex(hexbuf, sizeof(hexbuf),
                                         (unsigned long)uc->uc_mcontext.pc));
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                      " lr=%s", safe_hex(hexbuf, sizeof(hexbuf),
                                         (unsigned long)uc->uc_mcontext.regs[30]));
#elif defined(__arm__)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                      " pc=%s", safe_hex(hexbuf, sizeof(hexbuf),
                                         (unsigned long)uc->uc_mcontext.arm_pc));
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                      " lr=%s", safe_hex(hexbuf, sizeof(hexbuf),
                                         (unsigned long)uc->uc_mcontext.arm_lr));
#elif defined(__x86_64__)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                      " rip=%s", safe_hex(hexbuf, sizeof(hexbuf),
                                          (unsigned long)uc->uc_mcontext.gregs[REG_RIP]));
#elif defined(__i386__)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                      " eip=%s", safe_hex(hexbuf, sizeof(hexbuf),
                                          (unsigned long)uc->uc_mcontext.gregs[REG_EIP]));
#endif
    }
#else
    (void)uctx;
#endif
    buf[sizeof(buf) - 1] = '\0';

    /* Write to both stderr and syslog */
    (void)!write(STDERR_FILENO, buf, (size_t)n);
    (void)!write(STDERR_FILENO, "\n", 1);
    syslog(LOG_CRIT, "%s", buf);

#if HAVE_BACKTRACE
    void *frames[64];
    int nframes = backtrace(frames, 64);
    if (nframes > 0)
        backtrace_symbols_fd(frames, nframes, STDERR_FILENO);
#endif

    /* Re-raise with default disposition (SA_RESETHAND already restored it) */
    raise(sig);
}

/* Alternate signal stack so SIGSEGV from stack overflow can still run. */
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
    /* Force libgcc's lazy first-call malloc outside the handler. */
    void *dummy[1];
    (void)backtrace(dummy, 1);
#endif

    /* Install handler for fatal signals — SA_SIGINFO gives us ucontext
     * with register state (PC/LR) for post-mortem without frame pointers. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND | SA_NODEFER | SA_ONSTACK | SA_SIGINFO;

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGILL,  &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
}
