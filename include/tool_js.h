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

/* V38: Host context set as JSContext opaque — provides allowed_hosts to http_fetch binding */
typedef struct {
    int instruction_count;
    int instruction_limit;
    char **allowed_hosts;
    size_t allowed_hosts_count;
} JsHostCtx;

/* Set allowed_hosts on a persistent JS runtime (V38).
 * Does NOT take ownership — caller must keep hosts alive for runtime lifetime. */
void js_runtime_set_hosts(JsSessionRuntime *rt, char **hosts, size_t count);

/* Register js_eval tool into registry. Returns 0 on success. */
int tool_js_eval_register(ToolRegistry *reg);

/* Handler: parse JSON args {"code":"..."}, eval in sandboxed mquickjs.
 * V5: 1MB heap cap, 10M instruction limit.
 * Returns heap-allocated result string. */
char *tool_js_eval_handler(const char *arguments, void *user_data);

/* Context for js_define_tool (needs DB + session + registry + runtime) */
typedef struct {
    sqlite3 *db;
    int64_t session_id;
    ToolRegistry *reg;
    JsSessionRuntime *rt;
} JsDefineCtx;

/* Register js_define_tool into registry. Returns 0 on success. */
int tool_js_define_register(ToolRegistry *reg, JsDefineCtx *ctx);

/* Handler: parse JSON args {name, description, parameters, code},
 * persist to DB, register as callable tool. */
char *tool_js_define_handler(const char *arguments, void *user_data);

/* Create a persistent JS runtime for a session. Returns NULL on failure. */
JsSessionRuntime *js_runtime_create(void);

/* Destroy a session runtime (free heap). */
void js_runtime_destroy(JsSessionRuntime *rt);

/* Load all JS-defined tools for a session from DB into registry,
 * replaying definitions into the shared runtime context.
 * Call after session selection. Returns number loaded, or -1 on error. */
int tool_js_load_session(sqlite3 *db, int64_t session_id, ToolRegistry *reg,
                         JsSessionRuntime *rt);

#endif
