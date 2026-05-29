#ifndef CCLAW_PROXY_H
#define CCLAW_PROXY_H

#include <stddef.h>

/* V83: Credential proxy thread for shell children.
 * Listens on UDS at <workspace>/.proxy.sock.
 * Accepts connections, reads preamble (host:port or RESOLVE host),
 * checks allowed_hosts, resolves DNS, relays TCP bidirectionally.
 * Dies with agent process (thread is detached). */

typedef struct {
    char **allowed_hosts;   /* NULL or array of allowed hostnames */
    size_t allowed_count;   /* 0 = allow all */
    char *sock_path;        /* heap-allocated path to .proxy.sock */
    int listen_fd;          /* listening socket fd (-1 if not started) */
    int running;            /* set to 0 to stop */
} ProxyContext;

/* Start proxy thread. Creates UDS at <workspace>/.proxy.sock.
 * Returns 0 on success, -1 on failure.
 * Caller must call proxy_stop() before process exit. */
int proxy_start(ProxyContext *ctx, const char *workspace,
                char **allowed_hosts, size_t allowed_count);

/* Stop proxy thread and clean up socket file. */
void proxy_stop(ProxyContext *ctx);

/* Get the socket path (for setting CCLAW_PROXY_SOCK in shell children).
 * Returns NULL if proxy not started. */
const char *proxy_sock_path(const ProxyContext *ctx);

#endif
