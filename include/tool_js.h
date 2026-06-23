#ifndef CCLAW_TOOL_JS_H
#define CCLAW_TOOL_JS_H

#include "tools.h"
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

/* T104: Context for js_eval tool — carries per-agent allowed_hosts */
typedef struct {
    char **allowed_hosts;
    size_t allowed_hosts_count;
    int host_mode;  /* 1 = trust_level host (no sandbox), 0 = sandbox child */
    const char *trust_level;
    char **read_paths;
    size_t read_path_count;
    char **write_paths;
    size_t write_path_count;
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
 * shared store; it is fork+exec'd with the call args in scope when the tool
 * runs (same model as js_eval's filename mode). */
int js_tool_register_ext(ToolRegistry *reg, const char *name,
                         const char *description, const char *parameters_json,
                         const char *path, JsEvalCtx *ectx,
                         const char *policy_json);

#endif
