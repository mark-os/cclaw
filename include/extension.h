#ifndef CCLAW_EXTENSION_H
#define CCLAW_EXTENSION_H

#include <sqlite3.h>
#include "tool_js.h"
#include "tools.h"
#include "config.h"
#include <stddef.h>

/* V111: Hook event types */
typedef enum {
    HOOK_BEFORE_REQUEST,
    HOOK_AFTER_RESPONSE,
    HOOK_BEFORE_TOOL_CALL,
    HOOK_AFTER_TOOL_CALL,
    HOOK_TURN_START,
    HOOK_TURN_END,
    HOOK_EVENT_COUNT
} HookEvent;

/* V111/V112: Extension hooks stored as JS source strings.
 * Each hook is a function body stored as text — invoked via JS_Eval at dispatch time. */
typedef struct {
    char **fns;      /* Array of JS function ref expressions (e.g. "__hooks_beforeRequest[0]") */
    size_t count;
    size_t cap;
} HookList;

/* Extension context — owns hook registrations for the agent turn */
typedef struct {
    HookList hooks[HOOK_EVENT_COUNT];
    JsSessionRuntime *rt;
} ExtensionCtx;

/* T254: Discover extensions in workspace/extensions/.
 * Returns sorted array of absolute file paths. Caller frees via extension_list_free().
 * Scans .qjs files and subdir index.qjs files, sorted alphabetically. */
char **extension_discover(const char *workspace, size_t *count);
char **extension_discover_for_agent(sqlite3 *db, const char *agent_name,
                                    const char *workspace, size_t *count);
void extension_list_free(char **paths, size_t count);

/* T255/T256: Load discovered extensions into shared QuickJS context.
 * Installs cclaw API object, evals each extension, then processes
 * accumulated tool/hook registrations from JS into C structures.
 * Returns number of extensions successfully loaded. */
int extension_load(char **paths, size_t count, JsSessionRuntime *rt,
                   ToolRegistry *reg, const Config *cfg, ExtensionCtx *ext_ctx,
                   JsEvalCtx *ectx);

/* Initialize extension context. Call before extension_load. */
void extension_ctx_init(ExtensionCtx *ctx, JsSessionRuntime *rt);

/* Free extension context resources. */
void extension_ctx_destroy(ExtensionCtx *ctx);

/* Map event name string to HookEvent enum. Returns -1 if unknown. */
int hook_event_from_name(const char *name);

#endif
