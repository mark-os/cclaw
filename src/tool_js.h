#ifndef CCLAW_TOOL_JS_H
#define CCLAW_TOOL_JS_H

/* Default js_eval timeout, seconds; a call may raise it via the `timeout`
 * argument up to TOOL_TIMEOUT_MAX_SEC. */
#define JSEVAL_DEFAULT_TIMEOUT 120

/* The js_eval tool and extension tools — JS evaluation in the sandboxed
 * engine (the EXEC_JS vehicle). */

#include "tools.h"
#include "sandbox.h"
#include "run_tool.h"
#include <sqlite3.h>
#include <stdint.h>

/* Host context passed via env to forked QuickJS child. */
typedef struct {
    int instruction_count;
    int instruction_limit;
    char **allowed_hosts;
    size_t allowed_hosts_count;
} JsHostCtx;

/* Context for js_eval (EXEC_JS) — carries the containment, mirroring the
 * shell's. js runs in the same fork+execve --run-tool broker; its
 * http_request curl reaches the per-hop decide() proxy via HTTP_PROXY. Egress is
 * the proxy's job (allowed_hosts feed proxy_bind), not a pre-flight. */
typedef struct {
    char **allowed_hosts;        /* egress allowlist for the per-call proxy */
    size_t allowed_hosts_count;
    const char *workspace;
    const char *cwd_path;
    const char *db_path;         /* for the shared extension-store mount */
    Containment sb;              /* the agent's containment + grant paths */
} JsEvalCtx;

/* Register js_eval tool into registry. */
int tool_js_eval_register(ToolRegistry *reg, JsEvalCtx *ctx);

/* Handler: parse JSON args, eval in sandboxed QuickJS (fork+exec).
 * Returns heap-allocated result string. */
char *tool_js_eval_handler(const char *arguments, void *user_data, int *is_error);

/* Register an extension tool. `path` is the absolute handler .qjs file in the
 * shared store; the js broker evals it (filename mode) with the call args in
 * scope when the tool runs (same model as js_eval's filename mode). */
int js_tool_register_ext(ToolRegistry *reg, const char *name,
                         const char *description, const char *parameters_json,
                         const char *path, JsEvalCtx *ectx,
                         const char *policy_json);

/* Resolve a js tool entry (js_eval OR an extension tool) into its
 * containment and — for extension tools — the handler .qjs path the child runs
 * (*out_path=NULL for js_eval). Both outputs borrow the entry's storage. */
int js_tool_resolve_request(const ToolEntry *te, JsEvalCtx **out_ctx,
                            const char **out_path);

/* --run-tool child leaf: eval qjs in-process inside the broker's inner fork
 * (netns + proxy + mounts already applied). */
char *tool_js_child_run(const RunToolParsed *q, int *is_error);

#endif
