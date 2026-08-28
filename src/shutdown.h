#ifndef CCLAW_SHUTDOWN_H
#define CCLAW_SHUTDOWN_H

/* Install SIGINT/SIGTERM handlers via sigaction. Call once from main. */
void shutdown_init(void);

/* Returns non-zero if shutdown has been requested. */
int shutdown_requested(void);

/* Non-zero if the shutdown was requested by SIGUSR2, meaning "stop, then come
 * back as the binary now on disk". The daemon runs its normal shutdown either
 * way; this only decides whether it exits or re-execs at the end. */
int shutdown_reexec_requested(void);

#endif
