#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "qjs_helpers.h"
#include <stdlib.h>
#include <string.h>

/* Interrupt handler — counts operations, stops at limit.
 * Only increments when a limit is set (avoids overflow UB in unlimited mode). */
static int qjs_interrupt_handler(JSRuntime *rt, void *opaque) {
    (void)rt;
    QjsRuntime *qrt = (QjsRuntime *)opaque;
    if (qrt->instruction_limit <= 0) return 0;
    qrt->instruction_count++;
    return (qrt->instruction_count > qrt->instruction_limit);
}

QjsRuntime *qjs_runtime_create(size_t memory_limit) {
    QjsRuntime *qrt = calloc(1, sizeof(QjsRuntime));
    if (!qrt) return NULL;

    qrt->rt = JS_NewRuntime();
    if (!qrt->rt) { free(qrt); return NULL; }

    if (memory_limit > 0)
        JS_SetMemoryLimit(qrt->rt, memory_limit);
    JS_SetInterruptHandler(qrt->rt, qjs_interrupt_handler, qrt);
    return qrt;
}

void qjs_runtime_destroy(QjsRuntime *qrt) {
    if (!qrt) return;
    JS_FreeRuntime(qrt->rt);
    free(qrt);
}

void qjs_set_interrupt_limit(QjsRuntime *qrt, int max_instructions) {
    if (!qrt) return;
    qrt->instruction_limit = max_instructions;
    qrt->instruction_count = 0;
}

JSContext *qjs_context_create(QjsRuntime *qrt, QjsProfile profile) {
    if (!qrt) return NULL;

    JSContext *ctx;
    if (profile == QJS_PROFILE_HOOKS) {
        /* Minimal: base objects + eval + JSON + RegExp */
        ctx = JS_NewContextRaw(qrt->rt);
        if (!ctx) return NULL;
        JS_AddIntrinsicBaseObjects(ctx);
        JS_AddIntrinsicEval(ctx);
        JS_AddIntrinsicJSON(ctx);
        JS_AddIntrinsicRegExp(ctx);
        JS_AddIntrinsicRegExpCompiler(ctx);
    } else {
        /* Full context for eval and channel profiles */
        ctx = JS_NewContext(qrt->rt);
        if (!ctx) return NULL;
    }
    /* Reset instruction counter for this context's use */
    qrt->instruction_count = 0;
    return ctx;
}

char *qjs_eval_to_string(JSContext *ctx, const char *code, const char *filename) {
    if (!ctx || !code) return NULL;
    JSValue val = JS_Eval(ctx, code, strlen(code), filename ? filename : "<eval>",
                          JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(val))
        return qjs_get_exception_string(ctx);

    const char *str = JS_ToCString(ctx, val);
    JS_FreeValue(ctx, val);
    if (!str) return NULL;
    char *result = strdup(str);
    JS_FreeCString(ctx, str);
    return result;
}

int qjs_set_global_json(JSContext *ctx, const char *name, const char *json) {
    if (!ctx || !name || !json) return -1;
    JSValue parsed = JS_ParseJSON(ctx, json, strlen(json), "<json>");
    if (JS_IsException(parsed)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return -1;
    }
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, name, parsed);
    JS_FreeValue(ctx, global);
    return 0;
}

char *qjs_get_global_json(JSContext *ctx, const char *name) {
    if (!ctx || !name) return NULL;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue val = JS_GetPropertyStr(ctx, global, name);
    JS_FreeValue(ctx, global);

    if (JS_IsException(val) || JS_IsUndefined(val)) {
        JS_FreeValue(ctx, val);
        return NULL;
    }

    JSValue json = JS_JSONStringify(ctx, val, JS_UNDEFINED, JS_UNDEFINED);
    JS_FreeValue(ctx, val);
    if (JS_IsException(json)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return NULL;
    }

    const char *str = JS_ToCString(ctx, json);
    JS_FreeValue(ctx, json);
    if (!str) return NULL;
    char *result = strdup(str);
    JS_FreeCString(ctx, str);
    return result;
}

char *qjs_get_exception_string(JSContext *ctx) {
    if (!ctx) return NULL;
    JSValue exc = JS_GetException(ctx);
    if (JS_IsNull(exc) || JS_IsUndefined(exc)) {
        JS_FreeValue(ctx, exc);
        return strdup("unknown error");
    }
    const char *str = JS_ToCString(ctx, exc);
    JS_FreeValue(ctx, exc);
    if (!str) return strdup("unknown error");
    char *result = strdup(str);
    JS_FreeCString(ctx, str);
    return result;
}

void qjs_drain_jobs(JSRuntime *rt) {
    JSContext *c;
    while (JS_ExecutePendingJob(rt, &c) > 0) {}
}

JSValue qjs_unwrap_promise(JSContext *ctx, JSValue val) {
    if (JS_PromiseState(ctx, val) == JS_PROMISE_PENDING) {
        /* Not a promise at all, or genuinely pending — JS_PromiseState returns
         * JS_PROMISE_PENDING for non-promise values too, but JS_PromiseResult
         * would crash. Check via duck-typing: if it's not an object, pass through. */
    }
    /* JS_PromiseState returns -1 for non-promise values in some builds,
     * but the bellard 2026 version returns JS_PROMISE_PENDING. Safest check:
     * only unwrap if result is a fulfilled or rejected promise. */
    JSPromiseStateEnum state = JS_PromiseState(ctx, val);
    if (state == JS_PROMISE_FULFILLED) {
        JSValue result = JS_PromiseResult(ctx, val);
        JS_FreeValue(ctx, val);
        return JS_DupValue(ctx, result);
    }
    if (state == JS_PROMISE_REJECTED) {
        JS_FreeValue(ctx, val);
        return JS_UNDEFINED;
    }
    /* Not a promise or still pending — return as-is */
    return val;
}

void qjs_reset_instructions(QjsRuntime *qrt) {
    if (qrt) qrt->instruction_count = 0;
}
