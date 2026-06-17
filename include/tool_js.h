#ifndef CCLAW_TOOL_JS_H
#define CCLAW_TOOL_JS_H

#include "tools.h"
#include <sqlite3.h>
#include <stdint.h>

/* Persistent JS runtime for a session — shared context across tool calls */
typedef struct {
    void *heap;
    void *ctx;  /* JSContext* */
} JsSessionRuntime;

/* V38/V116: Host context set as JSContext opaque — provides allowed_hosts. */
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
    const char *trust_level;  /* propagated to forked child via CCLAW_TRUST_LEVEL */
} JsEvalCtx;

/* Set allowed_hosts on a persistent JS runtime (V38).
 * Does NOT take ownership — caller must keep hosts alive for runtime lifetime. */
void js_runtime_set_hosts(JsSessionRuntime *rt, char **hosts, size_t count);


/* Register js_eval tool into registry. ctx may be NULL (no hosts). */
int tool_js_eval_register(ToolRegistry *reg, JsEvalCtx *ctx);

/* Handler: parse JSON args {"code":"..."}, eval in sandboxed mquickjs.
 * V5: 1MB heap cap, 10M instruction limit.
 * Returns heap-allocated result string. */
char *tool_js_eval_handler(const char *arguments, void *user_data);


/* Create a persistent JS runtime for a session. Returns NULL on failure. */
JsSessionRuntime *js_runtime_create(void);

/* Destroy a session runtime (free heap). */
void js_runtime_destroy(JsSessionRuntime *rt);


/* T256: Register a JS tool from extension code.
 * Callable from extension.c. code is the JS function body receiving 'args'. */
int js_tool_register_ext(ToolRegistry *reg, const char *name,
                         const char *description, const char *parameters_json,
                         const char *code, JsEvalCtx *ectx);

#endif
