#ifndef CCLAW_QJS_HELPERS_H
#define CCLAW_QJS_HELPERS_H

/* Shared QuickJS runtime/context helpers: instruction-limit interrupts,
 * promise resolution/microtask draining, and the warning-suppressed
 * quickjs.h include. Common ground for every JS-hosting module.
 */

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
    QJS_PROFILE_CHANNEL,  /* cclaw --channel runner: full + Promise */
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
 * Caller frees. Test-only harness for qjs_resolve/qjs_get_exception_string
 * (test_quickjs) — production eval paths need the raw JSValue and inline
 * their own JS_Eval. */
char *qjs_eval_to_string(JSContext *ctx, const char *code, const char *filename);

/* Parse JSON string and set as a named global property. Returns 0 on success. */
int qjs_set_global_json(JSContext *ctx, const char *name, const char *json);

/* Stringify a named global property to JSON. Returns heap string or NULL.
 * Caller frees. */
char *qjs_get_global_json(JSContext *ctx, const char *name);

/* Extract exception message as heap string. Clears the exception. Caller frees. */
char *qjs_get_exception_string(JSContext *ctx);

/* Register eval-profile host functions (http_request, fs.*, console, print,
 * getConfig). `config_json` is the owning extension's settings object as JSON
 * (NULL/empty/unparseable → getConfig always returns null). */
void qjs_register_eval_host_functions(JSContext *ctx, const char *config_json);

/* Secrets available to http_request's fetch-boundary {{SECRET:name}}
 * resolution (borrowed pointers; caller keeps them alive across eval).
 * `hosts` is the secret's space-joined bound-host rules (NULL/empty =
 * unbound → any use fails closed). Set before qjs_eval_run; (NULL, 0)
 * clears. */
typedef struct {
    const char *name;
    const char *value;
    const char *hosts;
} JsHostSecret;
void qjs_host_set_secrets(const JsHostSecret *secrets, size_t count);

/* In-process JS evaluator (SBX_JS run_fn + unit-test entry). Runs the eval
 * profile with host functions in THIS process — the caller is responsible for
 * any sandbox/namespace setup beforehand (the broker child does it; unit tests
 * run host-mode in-process). Exactly one of `code` / `filename` is used; with
 * `filename`, `args_json` (may be NULL) is bound as `args`. `config_json` (may
 * be NULL) backs getConfig(). Returns a malloc'd result string (never NULL):
 * the last expression value, console output, or an "error: ..." message.
 * Egress (http_request) is via HTTP_PROXY, not a pre-flight. */
/* *is_error (may be NULL) carries the explicit outcome: a JS exception, a
 * bad request, or a runtime failure sets it — the "error:" wording in the
 * returned text is for the reader, not for a parser. */
char *qjs_eval_run(const char *code, const char *filename, const char *args_json,
                   const char *config_json, int *is_error);

/* Register channel-profile host functions (cclaw.*, admin.*). */
void qjs_register_channel_host_functions(JSContext *ctx);

/* Resolve an eval result. Drains the microtask queue to completion (running all
 * pending async work and settling promise chains), then unwraps a settled
 * top-level promise: fulfilled → its result (new ref); rejected → re-thrown so
 * the caller's exception path reports the reason; non-promise or still-pending →
 * returned unchanged. Mirrors quickjs-libc's js_std_loop + js_std_await, minus
 * the os_poll step (we expose no timers/IO, so promises settle via microtasks).
 * Call after JS_Eval on any value that isn't already an exception. */
JSValue qjs_resolve(JSContext *ctx, JSValue val);

/* Reset instruction counter — call before each handler dispatch in long-lived runtimes. */
void qjs_reset_instructions(QjsRuntime *qrt);

#endif
