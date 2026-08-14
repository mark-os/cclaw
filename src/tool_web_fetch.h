#ifndef CCLAW_TOOL_WEB_FETCH_H
#define CCLAW_TOOL_WEB_FETCH_H

#include "tools.h"
#include "sandbox.h"
#include "run_tool.h"

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
    int host_mode;               /* 1 = host trust, no egress enforcement */
    SandboxProfile sb;           /* trust-derived policy + grant paths */
} WebFetchCtx;

/* Register web_fetch tool into registry. ctx carries the sandbox profile. */
int tool_web_fetch_register(ToolRegistry *reg, WebFetchCtx *ctx);

/* Convert HTML to markdown (script/style dropped, tags → markers, entities
 * decoded). Returns a malloc'd string, caller frees. Non-HTML text passes
 * through mostly unchanged. */
char *html_to_markdown(const char *html, size_t html_len);

/* Actionable-denial hint: if enforcement is active (host_mode==0) and url's
 * host is not matched by rules, return a malloc'd "request grant_host" error
 * message; NULL otherwise. Pure — unit-testable without network. */
char *web_fetch_host_hint(const char *url, char **rules, size_t n, int host_mode);

/* --run-tool tier leaf: run web_fetch in-process inside the broker's inner
 * fork (netns + proxy already applied). */
char *tool_web_tier_run(const RunToolParsed *q, int *is_error);

#endif
