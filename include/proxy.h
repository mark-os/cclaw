#ifndef CCLAW_PROXY_H
#define CCLAW_PROXY_H

#include <pthread.h>
#include <stddef.h>

/* V83: Credential proxy thread for shell children.
 * Listens on UDS at <workspace>/.proxy.sock.
 * Accepts connections, reads preamble (host:port or RESOLVE host),
 * checks allowed_hosts, resolves DNS, relays TCP bidirectionally.
 * Accept-loop thread is joined in proxy_stop(). */

#define PROXY_BLESS_MAX 256
#define PROXY_BLESS_TTL_SECS 60

/* A resolution-blessed address: an IP that passed the allowlist + SSRF check
 * via a prior RESOLVE, or an explicitly granted literal IP. A numeric CONNECT
 * is permitted only if its address is present here and unexpired — this binds
 * the dialed IP to a prior allowed resolution (anti SSRF / DNS-rebinding). The
 * set is per-ProxyContext: process-lifetime while the proxy is a singleton,
 * per-call once the proxy moves into the broker. */
typedef struct {
    int family;                 /* AF_INET or AF_INET6 */
    unsigned char addr[16];     /* binary address (4 or 16 bytes used) */
    long expiry;                /* unix time when this bless expires */
} ProxyBlessedAddr;

typedef struct {
    char *db_path;          /* heap-allocated copy of DB path */
    char *agent_name;       /* heap-allocated copy of agent name */
    char *sock_path;        /* heap-allocated path to .proxy.sock */
    int listen_fd;          /* listening socket fd (-1 if not started) */
    int running;            /* set to 0 to stop */
    pthread_t thread;       /* accept-loop thread (valid if thread_started) */
    int thread_started;
    ProxyBlessedAddr blessed[PROXY_BLESS_MAX];  /* TTL-bound blessed-IP set */
    int blessed_count;
    pthread_mutex_t blessed_mu; /* guards blessed[]/blessed_count */
} ProxyContext;

/* Bind + listen on a per-call UDS at <workspace>/.proxy.<pid>.sock without
 * starting the accept thread. Lets the broker create the socket single-threaded,
 * fork the sandbox, then serve. Returns 0 on success. */
int proxy_bind(ProxyContext *ctx, const char *workspace,
               const char *db_path, const char *agent_name);

/* Start the accept-loop thread for a bound context. Returns 0 on success. */
int proxy_serve(ProxyContext *ctx);

/* Convenience: proxy_bind + proxy_serve. Creates UDS at <workspace>/.proxy.<pid>.sock.
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
