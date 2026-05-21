#ifndef CCLAW_TOOL_JS_H
#define CCLAW_TOOL_JS_H

#include "tools.h"

/* Register js_eval tool into registry. Returns 0 on success. */
int tool_js_eval_register(ToolRegistry *reg);

/* Handler: parse JSON args {"code":"..."}, eval in sandboxed mquickjs.
 * V5: 1MB heap cap, 10M instruction limit.
 * Returns heap-allocated result string. */
char *tool_js_eval_handler(const char *arguments, void *user_data);

#endif
