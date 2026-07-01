#ifndef CCLAW_HTTP_POLICY_H
#define CCLAW_HTTP_POLICY_H

/* Check if an IP string is private/loopback/link-local. Returns 1 if private.
 * The sole survivor of the old pre-flight HttpPolicy layer — egress for
 * web_fetch/js_eval/shell is now decided per-hop by the sandboxed proxy
 * (see proxy.c decide()/addr_permitted()), which is the only caller left. */
int http_is_private_ip(const char *ip);

#endif
