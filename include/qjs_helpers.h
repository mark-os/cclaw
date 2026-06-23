#ifndef CCLAW_QJS_HELPERS_H
#define CCLAW_QJS_HELPERS_H

/* Suppress unused-parameter warnings from quickjs.h inline functions */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include <quickjs.h>
#pragma GCC diagnostic pop

#include <stddef.h>

/* Execution profiles — determines which intrinsics are loaded */
typedef enum {
    QJS_PROFILE_HOOKS,    /* in-process hooks: Base + JSON + RegExp only */
    QJS_PROFILE_EVAL,     /* fork+exec js_eval: full (includes Proxy, Promise) */
    QJS_PROFILE_CHANNEL,  /* fork+exec channel_runner: full + Promise */
} QjsProfile;

/* Wrapper around JSRuntime with interrupt-based instruction limiting */
typedef struct {
    JSRuntime *rt;
    int instruction_count;
    int instruction_limit;  /* 0 = no limit (counter not incremented) */
} QjsRuntime;

/* Create a runtime with memory limit (bytes). 0 = no limit. */
QjsRuntime *qjs_runtime_create(size_t memory_limit);

/* Destroy runtime and free all associated memory. */
void qjs_runtime_destroy(QjsRuntime *qrt);

/* Set instruction limit (interrupt handler fires after N ops). 0 = disable. */
void qjs_set_interrupt_limit(QjsRuntime *qrt, int max_instructions);

/* Create a context with profile-appropriate intrinsics. */
JSContext *qjs_context_create(QjsRuntime *qrt, QjsProfile profile);

/* Eval code and return result as a heap-allocated string. NULL on error.
 * Caller frees. */
char *qjs_eval_to_string(JSContext *ctx, const char *code, const char *filename);

/* Parse JSON string and set as a named global property. Returns 0 on success. */
int qjs_set_global_json(JSContext *ctx, const char *name, const char *json);

/* Stringify a named global property to JSON. Returns heap string or NULL.
 * Caller frees. */
char *qjs_get_global_json(JSContext *ctx, const char *name);

/* Extract exception message as heap string. Clears the exception. Caller frees. */
char *qjs_get_exception_string(JSContext *ctx);

/* Register eval-profile host functions (http_request, fs.*, console, print). */
void qjs_register_eval_host_functions(JSContext *ctx);

/* Register channel-profile host functions (cclaw.*, admin.*). */
void qjs_register_channel_host_functions(JSContext *ctx);

/* Drain the microtask/job queue to completion. Call after any JS_Eval. */
void qjs_drain_jobs(JSRuntime *rt);

/* If val is a fulfilled Promise, return its result (new ref). If pending/rejected,
 * free val and return JS_UNDEFINED. Otherwise return val unchanged. */
JSValue qjs_unwrap_promise(JSContext *ctx, JSValue val);

/* Reset instruction counter — call before each handler dispatch in long-lived runtimes. */
void qjs_reset_instructions(QjsRuntime *qrt);

#endif
