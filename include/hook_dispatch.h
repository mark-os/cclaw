#ifndef CCLAW_HOOK_DISPATCH_H
#define CCLAW_HOOK_DISPATCH_H

#include "extension.h"
#include "context.h"
#include "config.h"
#include "tools.h"
#include <sqlite3.h>
#include <stdint.h>

/* V112: Dispatch beforeRequest hooks.
 * Loads entries from plan into JS messages array, calls each registered hook
 * in load order. Hooks receive mutable array, can modify/add/remove messages.
 * Returns heap-allocated full request body JSON if hooks modified messages,
 * or NULL if no hooks registered or messages unmodified. Caller frees. */
char *hook_dispatch_before_request(ExtensionCtx *ext_ctx, sqlite3 *db,
                                   int64_t session_id, const Config *cfg,
                                   const ContextPlan *plan,
                                   const ToolSchema *tools, size_t tool_count);

/* V113: Dispatch beforeToolCall hooks.
 * Calls each hook with {name, args}. Hook can return {block:true, reason} to
 * prevent execution. Returns heap-allocated error string if blocked, NULL if allowed.
 * Caller frees returned string. */
char *hook_dispatch_before_tool_call(ExtensionCtx *ext_ctx, sqlite3 *db,
                                     const char *name, const char *args);

/* V114: Dispatch afterToolCall hooks.
 * Calls each hook with {name, args, result}. Hook can return {result} to replace.
 * Returns replacement result (heap-allocated) if any hook replaced, NULL otherwise.
 * Caller frees returned string. */
char *hook_dispatch_after_tool_call(ExtensionCtx *ext_ctx, sqlite3 *db,
                                    const char *name, const char *args,
                                    const char *result);

/* V111/T260: Dispatch turnStart/turnEnd hooks. Informational — no return value. */
void hook_dispatch_turn_start(ExtensionCtx *ext_ctx);
void hook_dispatch_turn_end(ExtensionCtx *ext_ctx);

/* V111/T261: Dispatch afterResponse hook with LLM response (read-only inspect). */
void hook_dispatch_after_response(ExtensionCtx *ext_ctx, sqlite3 *db,
                                  const char *content,
                                  const char *finish_reason, int tool_call_count);

#endif
