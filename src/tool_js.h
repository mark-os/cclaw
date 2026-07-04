#ifndef CCLAW_TOOL_JS_H
#define CCLAW_TOOL_JS_H

#include "tools.h"
#include "sandbox.h"
#include "run_tool.h"
#include <sqlite3.h>
#include <stdint.h>

/* Persistent JS runtime for a session — shared context across extension loads */
typedef struct {
    void *heap;  /* QjsRuntime* */
    void *ctx;   /* JSContext* */
} JsSessionRuntime;

/* V38/V116: Host context passed via env to forked QuickJS child. */
typedef struct {
    int instruction_count;
    int instruction_limit;
    char **allowed_hosts;
    size_t allowed_hosts_count;
} JsHostCtx;

/* Context for js_eval (SBX_JS tier) — carries the sandbox profile, mirroring
 * the shell/web profile. js runs in the same fork+execve --run-tool broker; its
 * http_request curl reaches the per-hop decide() proxy via HTTP_PROXY. Egress is
 * the proxy's job (allowed_hosts feed proxy_bind), not a pre-flight. */
typedef struct {
    char **allowed_hosts;        /* egress allowlist for the per-call proxy */
    size_t allowed_hosts_count;
    int host_mode;               /* 1 = sandbox_profile host (sandbox=0), 0 = sandboxed */
    const char *sandbox_profile;
    const char *workspace;
    const char *cwd_path;
    const char *db_path;         /* for the shared extension-store mount */
    SandboxProfile sb;           /* trust-derived policy + grant paths */
} JsEvalCtx;

/* Set allowed_hosts on a persistent JS runtime (unused — hosts passed via env). */
void js_runtime_set_hosts(JsSessionRuntime *rt, char **hosts, size_t count);

/* Register js_eval tool into registry. */
int tool_js_eval_register(ToolRegistry *reg, JsEvalCtx *ctx);

/* Handler: parse JSON args, eval in sandboxed QuickJS (fork+exec).
 * Returns heap-allocated result string. */
char *tool_js_eval_handler(const char *arguments, void *user_data);

/* Create a persistent JS runtime for extension loading. */
JsSessionRuntime *js_runtime_create(void);

/* Destroy a session runtime. */
void js_runtime_destroy(JsSessionRuntime *rt);

/* Register an extension tool. `path` is the absolute handler .qjs file in the
 * shared store; the SBX_JS broker evals it (filename mode) with the call args in
 * scope when the tool runs (same model as js_eval's filename mode). */
int js_tool_register_ext(ToolRegistry *reg, const char *name,
                         const char *description, const char *parameters_json,
                         const char *path, JsEvalCtx *ectx,
                         const char *policy_json);

/* Resolve a JS-tier tool entry (js_eval OR an extension tool) into its sandbox
 * profile and the JSON eval request to ship in the SBX_JS blob. For js_eval the
 * args pass through; for an extension tool they are wrapped as
 * {"filename":<path>,"args":<args>}. Returns 0 on success; *out_args is malloc'd
 * (caller frees), *out_ctx borrows the entry's profile. */
int js_tool_resolve_request(const ToolEntry *te, const char *arguments,
                            JsEvalCtx **out_ctx, char **out_args);

/* --run-tool tier leaf: eval qjs in-process inside the broker's inner fork
 * (netns + proxy + mounts already applied). */
char *tool_js_tier_run(const RunToolParsed *q);

#endif
