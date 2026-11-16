/* Test qjs_helpers — the shared QuickJS helper layer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qjs_helpers.h"
#include "test_util.h"

static int failures = 0;
#define ASSERT(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while(0)

static void test_context_intrinsics(void) {
    QjsRuntime *qrt = qjs_runtime_create(2 * 1024 * 1024);

    /* One context shape for every JS host: the full intrinsic set. */
    JSContext *ctx = qjs_context_create(qrt);
    ASSERT(ctx != NULL, "context create");
    char *r = qjs_eval_to_string(ctx, "JSON.stringify({a:1})", NULL);
    ASSERT(r && strcmp(r, "{\"a\":1}") == 0, "context has JSON");
    free(r);
    r = qjs_eval_to_string(ctx, "new Date(0).toISOString()", NULL);
    ASSERT(r && strcmp(r, "1970-01-01T00:00:00.000Z") == 0, "context has Date");
    free(r);
    r = qjs_eval_to_string(ctx, "typeof Promise", NULL);
    ASSERT(r && strcmp(r, "function") == 0, "context has Promise");
    free(r);
    JS_FreeContext(ctx);

    qjs_runtime_destroy(qrt);
}

static void test_eval_to_string(void) {
    QjsRuntime *qrt = qjs_runtime_create(1024 * 1024);
    JSContext *ctx = qjs_context_create(qrt);

    char *r = qjs_eval_to_string(ctx, "2 + 2", NULL);
    ASSERT(r && strcmp(r, "4") == 0, "eval 2+2");
    free(r);

    r = qjs_eval_to_string(ctx, "'hello'", NULL);
    ASSERT(r && strcmp(r, "hello") == 0, "eval string");
    free(r);

    /* Error case */
    r = qjs_eval_to_string(ctx, "throw new Error('boom')", NULL);
    ASSERT(r && strstr(r, "boom"), "eval error contains message");
    free(r);

    JS_FreeContext(ctx);
    qjs_runtime_destroy(qrt);
}

static void test_console_log_circular(void) {
    QjsRuntime *qrt = qjs_runtime_create(2 * 1024 * 1024);
    JSContext *ctx = qjs_context_create(qrt);
    qjs_register_eval_host_functions(ctx, NULL);

    /* Prelude: create __console_buf */
    JS_Eval(ctx, "var __console_buf = [];", 23, "<pre>", JS_EVAL_TYPE_GLOBAL);

    /* Circular ref should not leave a pending exception */
    char *r = qjs_eval_to_string(ctx, "var a = {}; a.self = a; console.log(a); 'ok'", NULL);
    ASSERT(r && strcmp(r, "ok") == 0, "console.log circular does not corrupt context");
    free(r);

    /* Subsequent eval must still work */
    r = qjs_eval_to_string(ctx, "2 + 2", NULL);
    ASSERT(r && strcmp(r, "4") == 0, "eval after circular console.log");
    free(r);

    JS_FreeContext(ctx);
    qjs_runtime_destroy(qrt);
}

static void test_promise_resolve(void) {
    QjsRuntime *qrt = qjs_runtime_create(2 * 1024 * 1024);
    JSContext *ctx = qjs_context_create(qrt);

    /* async fn result is unwrapped, not "[object Promise]" */
    char *r = qjs_eval_to_string(ctx, "(async function(){ return 1 + 2; })()", NULL);
    ASSERT(r && strcmp(r, "3") == 0, "async fn result unwrapped to 3");
    free(r);

    /* .then continuation (a microtask) runs */
    r = qjs_eval_to_string(ctx, "Promise.resolve(40).then(function(x){ return x + 2; })", NULL);
    ASSERT(r && strcmp(r, "42") == 0, "promise.then chain resolved to 42");
    free(r);

    /* multi-stage await: every continuation drains before we read the value */
    r = qjs_eval_to_string(ctx,
        "(async function(){ var a = await 10; var b = await 20; return a + b; })()", NULL);
    ASSERT(r && strcmp(r, "30") == 0, "multi-stage await resolved to 30");
    free(r);

    /* rejected promise surfaces the reason as an error (not undefined) */
    r = qjs_eval_to_string(ctx, "Promise.reject(new Error('nope'))", NULL);
    ASSERT(r && strstr(r, "nope"), "rejected promise reports reason");
    free(r);

    /* async throw lands on the same error path */
    r = qjs_eval_to_string(ctx, "(async function(){ throw new Error('async boom'); })()", NULL);
    ASSERT(r && strstr(r, "async boom"), "async throw reports reason");
    free(r);

    /* context is still usable after a rejection */
    r = qjs_eval_to_string(ctx, "2 + 2", NULL);
    ASSERT(r && strcmp(r, "4") == 0, "eval works after rejection");
    free(r);

    /* non-promise value passes through unchanged */
    r = qjs_eval_to_string(ctx, "6 * 7", NULL);
    ASSERT(r && strcmp(r, "42") == 0, "non-promise passes through");
    free(r);

    JS_FreeContext(ctx);
    qjs_runtime_destroy(qrt);
}

static void test_interrupt_limit(void) {
    QjsRuntime *qrt = qjs_runtime_create(1024 * 1024);
    qjs_set_interrupt_limit(qrt, 100);
    JSContext *ctx = qjs_context_create(qrt);

    /* Infinite loop should be interrupted */
    char *r = qjs_eval_to_string(ctx, "while(true){}", NULL);
    ASSERT(r != NULL, "interrupt returns error string");
    ASSERT(strstr(r, "interrupted") || strstr(r, "Interrupt"), "interrupt message");
    free(r);

    JS_FreeContext(ctx);
    qjs_runtime_destroy(qrt);
}

/* An exception is reported as what was thrown — a null stays "null". */
static void test_throw_null(void) {
    QjsRuntime *qrt = qjs_runtime_create(1024 * 1024);
    JSContext *ctx = qjs_context_create(qrt);

    char *r = qjs_eval_to_string(ctx, "throw null", NULL);
    ASSERT(r && strcmp(r, "null") == 0, "thrown null reads null");
    free(r);

    JS_FreeContext(ctx);
    qjs_runtime_destroy(qrt);
}

/* Atomics is compiled out of the vendored QuickJS (see CONFIG_ATOMICS in
 * quickjs.c): no runtime here ever has a second thread to synchronise with,
 * and its 64-bit operations were the only thing needing libatomic on ARMv5TE.
 * A QuickJS update that silently turns it back on should fail here, not in an
 * armv5te link. Shared memory itself stays. */
static void test_no_atomics(void) {
    QjsRuntime *qrt = qjs_runtime_create(1024 * 1024);
    JSContext *ctx = qjs_context_create(qrt);

    char *r = qjs_eval_to_string(ctx,
        "var a = new BigInt64Array(new SharedArrayBuffer(16));"
        "a[0] = 0x123456789abcdefn;"
        "[typeof Atomics, a[0] === 0x123456789abcdefn].join()", NULL);
    ASSERT(r && strcmp(r, "undefined,true") == 0, "Atomics absent, 64-bit arrays work");
    free(r);

    JS_FreeContext(ctx);
    qjs_runtime_destroy(qrt);
}

int main(void) {
    TEST_INIT();
    test_context_intrinsics();
    test_eval_to_string();
    test_console_log_circular();
    test_promise_resolve();
    test_interrupt_limit();
    test_throw_null();
    test_no_atomics();

    if (failures == 0)
        printf("PASS: test_quickjs (%d checks)\n", 0);
    else
        printf("FAIL: test_quickjs (%d failures)\n", failures);
    return failures ? 1 : 0;
}
