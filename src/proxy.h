#ifndef CCLAW_PROXY_H
#define CCLAW_PROXY_H

#include <pthread.h>
#include <stddef.h>
#include "host_match.h"

/* Credential proxy thread for shell children.
 * Listens on a per-call pathname UDS at <agent-folder>/.proxy.<pid>.sock — the
 * agent folder, never the agent-visible workspace. The sandbox bind-mounts the
 * socket to a fixed in-netns path so the in-sandbox preload/shim can reach it.
 * Accepts connections, reads preamble (host:port or RESOLVE host), checks the
 * allowlist, resolves DNS, relays TCP bidirectionally.
 * Accept-loop thread is joined in proxy_stop(). */

#define PROXY_BLESS_MAX 256
#define PROXY_BLESS_TTL_SECS 60

/* Per-call cap on simultaneous relays — bounds broker-side fd consumption when a
 * pooling HTTP client opens many CONNECT tunnels. Leaves headroom under the
 * broker's setrlimit fd cap. */
#define PROXY_MAX_RELAYS 64

/* Cap on total in-flight conn_threads (resolving + relaying), independent of
 * PROXY_MAX_RELAYS which only bounds post-resolve relays. Bounds the blast
 * radius of a hostname whose DNS never responds — cheaper than adding async
 * DNS (no portable getaddrinfo_a on musl/ARMv5TE targets). */
#define PROXY_MAX_PENDING 128

/* Cap on distinct contacted hosts remembered per call (network_hosts tag). */
#define PROXY_CONTACTED_MAX 64

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
    int via_hostname;           /* blessed by hostname resolution — CONNECTs to
                                   it get the Q6 SNI gate like the hostname path */
} ProxyBlessedAddr;

typedef struct {
    char **hosts;           /* borrowed egress allowlist (NULL ⇒ deny all) */
    size_t host_count;      /* must outlive the proxy (per-call caps->hosts) */
    char **host_rules;      /* partitioned: hostname rules (exact/suffix) */
    size_t host_rule_count;
    Cidr *cidr_rules;       /* partitioned: CIDR + literal-IP rules (owned) */
    size_t cidr_rule_count;
    char **deny_rules;      /* borrowed: sensitive-host deny labels (checked before allow) */
    size_t deny_count;
    char *sock_path;        /* heap-allocated path to .proxy.sock */
    int listen_fd;          /* listening socket fd (-1 if not started) */
    int running;            /* set to 0 to stop */
    pthread_t thread;       /* accept-loop thread (valid if thread_started) */
    int thread_started;
    ProxyBlessedAddr blessed[PROXY_BLESS_MAX];  /* TTL-bound blessed-IP set */
    int blessed_count;
    int relay_count;            /* live relays (guarded by blessed_mu) */
    pthread_mutex_t blessed_mu; /* guards blessed[]/blessed_count/relay_count */
    int conn_active;            /* in-flight conn_threads (guarded by blessed_mu) */
    pthread_cond_t conn_cond;   /* signalled when conn_active reaches 0 */
    char *contacted[PROXY_CONTACTED_MAX]; /* dedup'd allowed targets (owned) */
    int contacted_count;                  /* guarded by blessed_mu */
    char *denied[PROXY_CONTACTED_MAX];    /* dedup'd denied targets (owned) */
    int denied_count;                     /* guarded by blessed_mu */
    unsigned short sni_strict_port; /* port where a parsed ClientHello lacking
                                       SNI is denied (D7: modern HTTPS always
                                       sends SNI; omitting it is the trivial
                                       gate evasion). 443 by default; tests
                                       point it at a loopback sink port. */
} ProxyContext;

/* Bind + listen on a per-call UDS at <dir>/.proxy.<pid>.sock (dir = the agent
 * folder) without starting the accept thread. Lets the broker create the socket
 * single-threaded, fork the sandbox, then serve. The egress allowlist
 * (`hosts`/`host_count`) is borrowed — it must outlive the proxy; the broker
 * holds no DB handle. `deny_rules`/`deny_count` are sensitive-host labels
 * (borrowed) checked before any allow logic — deny wins unconditionally.
 * Returns 0 on success. */
int proxy_bind(ProxyContext *ctx, const char *dir,
               char **hosts, size_t host_count,
               char **deny_rules, size_t deny_count);

/* Start the accept-loop thread for a bound context. Returns 0 on success. */
int proxy_serve(ProxyContext *ctx);

/* Stop proxy thread and clean up socket file. */
void proxy_stop(ProxyContext *ctx);

/* Get the socket path (for setting CCLAW_PROXY_SOCK in shell children).
 * Returns NULL if proxy not started. */
const char *proxy_sock_path(const ProxyContext *ctx);

/* JSON array of the hosts this call was allowed to contact (e.g.
 * ["api.github.com"]), malloc'd; NULL if nothing was contacted. Call before
 * proxy_stop() — stop frees the collected list. */
char *proxy_hosts_json(ProxyContext *ctx);

/* Human-readable summary of hosts denied during this call, malloc'd; NULL if
 * none were denied. Intended for appending to tool results so the model knows
 * which hosts were blocked by the proxy. advice overrides the trailing
 * request-a-grant hint (NULL = generic grants.hosts wording) — a narrowed
 * call passes the parent-composed binding note. Call before proxy_stop(). */
char *proxy_denied_summary(ProxyContext *ctx, const char *advice);

#endif
