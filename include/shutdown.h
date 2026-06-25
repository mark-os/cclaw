#ifndef CCLAW_SHUTDOWN_H
#define CCLAW_SHUTDOWN_H

/* Install SIGINT/SIGTERM handlers via sigaction. Call once from main. */
void shutdown_init(void);

/* Returns non-zero if shutdown has been requested. */
int shutdown_requested(void);

/* Programmatically request shutdown (e.g., from tests). */
void shutdown_request(void);

#endif
