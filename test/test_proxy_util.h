#ifndef CCLAW_TEST_PROXY_UTIL_H
#define CCLAW_TEST_PROXY_UTIL_H

#include "../src/proxy.h"

/* Test-only convenience wrapper: proxy_bind + proxy_serve in one call.
 * Returns 0 on success, -1 on failure (calls proxy_stop on partial init). */
static inline int proxy_start(ProxyContext *ctx, const char *dir,
                              char **hosts, size_t host_count) {
    if (proxy_bind(ctx, dir, hosts, host_count) != 0)
        return -1;
    if (proxy_serve(ctx) != 0) {
        proxy_stop(ctx);
        return -1;
    }
    return 0;
}

#endif /* CCLAW_TEST_PROXY_UTIL_H */
