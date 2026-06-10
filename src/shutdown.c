#define _POSIX_C_SOURCE 200809L
#include "shutdown.h"
#include <signal.h>
#include <stddef.h>

static volatile sig_atomic_t g_shutdown = 0;

static void handle_signal(int sig) {
    (void)sig;
    g_shutdown = 1;
}

void shutdown_init(void) {
    struct sigaction sa = {0};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Ignore SIGPIPE — let write() return EPIPE instead of crashing */
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
}

int shutdown_requested(void) {
    return g_shutdown;
}

