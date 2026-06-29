#ifndef CCLAW_TOOL_WEB_FETCH_H
#define CCLAW_TOOL_WEB_FETCH_H

#include "tools.h"
#include "http_policy.h"
#include "sandbox.h"

/* Sandbox-profile context for web_fetch (SBX_WEB tier). Mirrors the shell/file
 * profile: web runs in the same fork+execve --run-tool broker, its curl reaching
 * the per-hop decide() proxy via HTTP_PROXY (no inner exec). Egress is decided
 * by the proxy from `allowed_hosts`, NOT a pre-flight — there is no HttpPolicy
 * preflight anymore (a single check can't see redirects). */
typedef struct {
    const char *workspace;
    const char *cwd_path;
    const char *db_path;
    char **allowed_hosts;        /* egress allowlist for the per-call proxy */
    size_t allowed_host_count;
    SandboxProfile sb;           /* trust-derived policy + grant paths */
} WebFetchCtx;

/* Register web_fetch tool into registry. ctx carries the sandbox profile. */
int tool_web_fetch_register(ToolRegistry *reg, WebFetchCtx *ctx);

/* Handler: parse JSON args {"url":"..."}, HTTP GET, strip HTML, wrap in
 * external input protection boundary markers (V15). Runs INSIDE the sandbox
 * broker child; egress is enforced by the proxy, so user_data is unused. */
char *tool_web_fetch_handler(const char *arguments, void *user_data);

/* Strip HTML tags from src, write plain text to dst. Returns bytes written. */
size_t html_strip_tags(const char *src, char *dst, size_t dst_cap);

#endif
