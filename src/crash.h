#ifndef CCLAW_CRASH_H
#define CCLAW_CRASH_H

/* Install fatal-signal handlers (SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE).
 * Logs signal + backtrace (glibc only) to stderr, then re-raises so
 * core-dump behavior is preserved. Call once from main after shutdown_init. */
void crash_handler_init(void);

#endif
