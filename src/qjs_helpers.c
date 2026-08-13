#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "qjs_helpers.h"
#include <stdio.h>
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
    if (!JS_IsException(val))
        val = qjs_resolve(ctx, val);   /* drain microtasks; await/unwrap promise */
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

/* A pending exception that is null/undefined, or one that will not convert to
 * a string, has exactly one cause in practice: the engine ran out of memory
 * before it could build the Error object. QuickJS is explicit about the first
 * case — JS_ThrowError2 substitutes JS_NULL when the allocation fails ("out of
 * memory: throw JS_NULL to avoid recursing"), and the second is the same wall
 * one step later. Saying "unknown error" here loses the one fact the agent
 * needs: an agent told its input is too large shrinks it, an agent told
 * "unknown" retries the same call. */
static const char *OOM_BEFORE_ERROR_OBJ =
    "out of memory (heap exhausted before the error object could be built)";

char *qjs_get_exception_string(JSContext *ctx) {
    if (!ctx) return NULL;
    JSValue exc = JS_GetException(ctx);
    if (JS_IsNull(exc) || JS_IsUndefined(exc)) {
        JS_FreeValue(ctx, exc);
        return strdup(OOM_BEFORE_ERROR_OBJ);
    }
    const char *str = JS_ToCString(ctx, exc);
    JS_FreeValue(ctx, exc);
    if (!str) return strdup(OOM_BEFORE_ERROR_OBJ);
    char *result = strdup(str);
    JS_FreeCString(ctx, str);
    return result;
}

JSValue qjs_resolve(JSContext *ctx, JSValue val) {
    JSRuntime *rt = JS_GetRuntime(ctx);

    /* Drain the whole microtask queue (js_std_loop's inner loop): runs every
     * pending job and settles all promise chains. A side job that throws leaves
     * its exception pending on the realm context — report and clear it so it
     * can't poison the unwrap or the caller, then keep draining. */
    for (;;) {
        int err = JS_ExecutePendingJob(rt, NULL);
        if (err == 0) break;
        if (err < 0) {
            char *msg = qjs_get_exception_string(ctx);
            /* stderr on purpose: in the qjs_eval child stderr is captured into
             * the tool result, so the model sees the job error. */
            if (msg) { fprintf(stderr, "[qjs] job error: %s\n", msg); free(msg); }
        }
    }

    /* Unwrap the now-settled top-level promise (js_std_await's terminal cases).
     * JS_PromiseState returns -1 for non-promise values, so -1 and a still-
     * pending 0 both fall through unchanged. */
    switch (JS_PromiseState(ctx, val)) {
    case JS_PROMISE_FULFILLED: {
        /* JS_PromiseResult already returns a new ref — transfer it, don't re-dup. */
        JSValue result = JS_PromiseResult(ctx, val);
        JS_FreeValue(ctx, val);
        return result;
    }
    case JS_PROMISE_REJECTED: {
        /* Re-throw the reason so the caller's normal exception path reports it. */
        JSValue exc = JS_Throw(ctx, JS_PromiseResult(ctx, val));
        JS_FreeValue(ctx, val);
        return exc;
    }
    default:
        return val;
    }
}

void qjs_reset_instructions(QjsRuntime *qrt) {
    if (qrt) qrt->instruction_count = 0;
}
