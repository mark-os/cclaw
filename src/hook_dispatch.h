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

/* §8 gating hook — restrict-only decision, most-restrictive wins. */
typedef enum { HOOK_GATE_ALLOW = 0, HOOK_GATE_ASK = 1, HOOK_GATE_DENY = 2 } HookGate;

/* §8 Gating hook (event "beforeToolCall"): fires once, after capability
 * resolution, before execution. Calls each hook with {name, args}. A hook may
 * only *restrict*: returning {deny:true,reason} (or {block:true}) → DENY,
 * {ask:true,reason} → ASK, anything else → ALLOW. The most restrictive decision
 * across all hooks wins — a hook can veto, never authorize. On DENY/ASK,
 * *reason_out (if non-NULL) receives a heap reason string (caller frees; may be
 * left NULL). Returns HOOK_GATE_ALLOW when no gating hooks are registered. */
HookGate hook_dispatch_gate_tool_call(ExtensionCtx *ext_ctx, sqlite3 *db,
                                      const char *name, const char *args,
                                      char **reason_out);

/* §8 Observer hook (event "afterToolCall"): fires once, after execution.
 * Side-effect only — each hook is called with {name, args, result} and any
 * return value is ignored. An observer can audit/notify/react but cannot gate
 * or modify the result (the action already happened). */
void hook_dispatch_observe_tool_call(ExtensionCtx *ext_ctx, sqlite3 *db,
                                     const char *name, const char *args,
                                     const char *result);

/* V111/T260: Dispatch turnStart/turnEnd hooks. Informational — no return value. */

#endif
