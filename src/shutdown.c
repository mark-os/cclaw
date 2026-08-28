#define _POSIX_C_SOURCE 200809L
#include "shutdown.h"
#include <signal.h>
#include <stddef.h>

static volatile sig_atomic_t g_shutdown = 0;
static volatile sig_atomic_t g_reexec = 0;

static void handle_signal(int sig) {
    /* SIGUSR2 means "restart into the binary now on disk". It takes the same
     * path as a normal shutdown — drain workers, stop channels, close the DB —
     * and only differs in the last step, so there is no second lifecycle to
     * keep correct. */
    if (sig == SIGUSR2) g_reexec = 1;
    g_shutdown = 1;
}

void shutdown_init(void) {
    struct sigaction sa = {0};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);

    /* Ignore SIGPIPE — let write() return EPIPE instead of crashing */
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
}

int shutdown_requested(void) {
    return g_shutdown;
}

int shutdown_reexec_requested(void) {
    return g_reexec;
}

