#ifndef CCLAW_EXTENSION_H
#define CCLAW_EXTENSION_H

#include <sqlite3.h>
#include "tool_js.h"
#include "tools.h"
#include "config.h"
#include <stddef.h>

/* V111: Hook event types. preAdvance/postAdvance replace the dead
 * beforeRequest/afterResponse (specs/hooks.md); turnStart/turnEnd are declared
 * but not yet dispatched. */
typedef enum {
    HOOK_PRE_ADVANCE,
    HOOK_POST_ADVANCE,
    HOOK_BEFORE_TOOL_CALL,
    HOOK_AFTER_TOOL_CALL,
    HOOK_TURN_START,
    HOOK_TURN_END,
    HOOK_EVENT_COUNT
} HookEvent;

/* Extension hooks stored as JS source strings. Each entry is a handler file's
 * contents (a function expression) — re-evaluated in a fresh QuickJS context at
 * dispatch time (hook_dispatch.c). */
typedef struct {
    char **fns;      /* Array of function-expression source strings */
    size_t count;
    size_t cap;
} HookList;

/* Extension context — owns hook registrations for the agent turn */
typedef struct {
    HookList hooks[HOOK_EVENT_COUNT];
    JsSessionRuntime *rt;
} ExtensionCtx;

/* Discover *draft* extensions in workspace/extensions/ (authoring working
 * copies, not yet promoted). Returns a sorted array of absolute file paths;
 * caller frees via extension_list_free(). Used for listing drafts. */
char **extension_discover(const char *workspace, size_t *count);
void extension_list_free(char **paths, size_t count);

/* Load this agent's hooks from the DB `hooks` table (join of attached, enabled,
 * visible extensions) into ext_ctx->hooks[event]. Each handler file's source is
 * read and stored for fresh-context dispatch. No JS is evaluated here. Returns
 * the number of hooks loaded. */
int extension_load_hooks(ExtensionCtx *ctx, sqlite3 *db, const char *agent_name);

/* Initialize extension context. Call before loading hooks. */
void extension_ctx_init(ExtensionCtx *ctx, JsSessionRuntime *rt);

/* Free extension context resources. */
void extension_ctx_destroy(ExtensionCtx *ctx);

/* Map event name string to HookEvent enum. Returns -1 if unknown. */
int hook_event_from_name(const char *name);

#endif
