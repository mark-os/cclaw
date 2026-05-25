#ifndef CCLAW_HTTP_POLICY_H
#define CCLAW_HTTP_POLICY_H

#include <stddef.h>

/* V46: HTTP policy layer — validates hostname before connect.
 * NULL policy = unrestricted (used for LLM/telegram calls). */
typedef struct {
    char **allowed_hosts;     /* NULL or array of allowed hostnames */
    size_t allowed_count;
    char **blocked_hosts;     /* NULL or array of blocked hostnames */
    size_t blocked_count;
    int block_private;        /* reject RFC1918/loopback/link-local IPs */
} HttpPolicy;

/* Check URL against policy. Returns 0 if allowed, -1 if denied.
 * On denial, writes reason to err_buf (if non-NULL).
 * NULL policy = always allowed. */
int http_check_policy(const char *url, const HttpPolicy *policy,
                      char *err_buf, size_t err_cap);

/* Extract hostname from URL into host buffer. Returns 0 on success. */
int http_extract_host(const char *url, char *host, size_t host_cap);

/* Check if an IP string is private/loopback/link-local. Returns 1 if private. */
int http_is_private_ip(const char *ip);

#endif
