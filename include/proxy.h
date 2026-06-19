#ifndef CCLAW_PROXY_H
#define CCLAW_PROXY_H

#include <pthread.h>
#include <stddef.h>

/* V83: Credential proxy thread for shell children.
 * Listens on UDS at <workspace>/.proxy.sock.
 * Accepts connections, reads preamble (host:port or RESOLVE host),
 * checks allowed_hosts, resolves DNS, relays TCP bidirectionally.
 * Accept-loop thread is joined in proxy_stop(). */

typedef struct {
    char *db_path;          /* heap-allocated copy of DB path */
    char *agent_name;       /* heap-allocated copy of agent name */
    char *sock_path;        /* heap-allocated path to .proxy.sock */
    int listen_fd;          /* listening socket fd (-1 if not started) */
    int running;            /* set to 0 to stop */
    pthread_t thread;       /* accept-loop thread (valid if thread_started) */
    int thread_started;
} ProxyContext;

/* Start proxy thread. Creates UDS at <workspace>/.proxy.sock.
 * Returns 0 on success, -1 on failure.
 * Caller must call proxy_stop() before process exit. */
int proxy_start(ProxyContext *ctx, const char *workspace,
                const char *db_path, const char *agent_name);

/* Stop proxy thread and clean up socket file. */
void proxy_stop(ProxyContext *ctx);

/* Get the socket path (for setting CCLAW_PROXY_SOCK in shell children).
 * Returns NULL if proxy not started. */
const char *proxy_sock_path(const ProxyContext *ctx);

#endif
