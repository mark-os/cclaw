#ifndef CCLAW_TOOL_JS_H
#define CCLAW_TOOL_JS_H

#include "tools.h"
#include <sqlite3.h>
#include <stdint.h>

/* Register js_eval tool into registry. Returns 0 on success. */
int tool_js_eval_register(ToolRegistry *reg);

/* Handler: parse JSON args {"code":"..."}, eval in sandboxed mquickjs.
 * V5: 1MB heap cap, 10M instruction limit.
 * Returns heap-allocated result string. */
char *tool_js_eval_handler(const char *arguments, void *user_data);

/* Context for js_define_tool (needs DB + session + registry) */
typedef struct {
    sqlite3 *db;
    int64_t session_id;
    ToolRegistry *reg;
} JsDefineCtx;

/* Register js_define_tool into registry. Returns 0 on success. */
int tool_js_define_register(ToolRegistry *reg, JsDefineCtx *ctx);

/* Handler: parse JSON args {name, description, parameters, code},
 * persist to DB, register as callable tool. */
char *tool_js_define_handler(const char *arguments, void *user_data);

/* Load all JS-defined tools for a session from DB into registry.
 * Call after session selection. Returns number loaded, or -1 on error. */
int tool_js_load_session(sqlite3 *db, int64_t session_id, ToolRegistry *reg);

#endif
