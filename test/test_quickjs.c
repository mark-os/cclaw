/* qjs_helpers, the shared QuickJS helper layer. CI also runs this binary on
 * each cross-built architecture as the JS-engine smoke test. */
#include "qjs_helpers.h"
#include "test_util.h"
#include <assert.h>

static char *eval(JSContext *ctx, const char *code) {
    return qjs_eval_to_string(ctx, code, NULL);
}

static void expect(JSContext *ctx, const char *code, const char *want, int exact) {
    char *r = eval(ctx, code);
    int ok = r && (exact ? strcmp(r, want) == 0 : strstr(r, want) != NULL);
    if (!ok) fprintf(stderr, "eval %s: got %s, want %s\n", code, r ? r : "(null)", want);
    assert(ok);
    free(r);
}

/* Smoke: values, a thrown error (its message, and a thrown null reads null),
 * and the context survives the error. */
static void test_eval_to_string(void) {
    QjsRuntime *qrt = qjs_runtime_create(2 * 1024 * 1024);
    JSContext *ctx = qjs_context_create(qrt);
    assert(ctx);
    expect(ctx, "2 + 2", "4", 1);
    expect(ctx, "JSON.stringify({a:1})", "{\"a\":1}", 1);
    expect(ctx, "throw new Error('boom')", "boom", 0);
    expect(ctx, "throw null", "null", 1);
    expect(ctx, "'still' + ' alive'", "still alive", 1);
    JS_FreeContext(ctx);
    qjs_runtime_destroy(qrt);
}

/* Our eval drains the job queue: an async result is unwrapped, not
 * "[object Promise]", and a rejection surfaces its reason. */
static void test_promise_resolve(void) {
    QjsRuntime *qrt = qjs_runtime_create(2 * 1024 * 1024);
    JSContext *ctx = qjs_context_create(qrt);
    expect(ctx, "(async function(){ var a = await 10; var b = await 20; return a + b; })()",
           "30", 1);
    expect(ctx, "Promise.reject(new Error('nope'))", "nope", 0);
    expect(ctx, "2 + 2", "4", 1);
    JS_FreeContext(ctx);
    qjs_runtime_destroy(qrt);
}

/* console.log of a circular object must not leave a pending exception that
 * corrupts the next eval. */
static void test_console_log_circular(void) {
    QjsRuntime *qrt = qjs_runtime_create(2 * 1024 * 1024);
    JSContext *ctx = qjs_context_create(qrt);
    qjs_register_eval_host_functions(ctx, NULL);
    JS_Eval(ctx, "var __console_buf = [];", 23, "<pre>", JS_EVAL_TYPE_GLOBAL);
    expect(ctx, "var a = {}; a.self = a; console.log(a); 'ok'", "ok", 1);
    expect(ctx, "2 + 2", "4", 1);
    JS_FreeContext(ctx);
    qjs_runtime_destroy(qrt);
}

static void test_interrupt_limit(void) {
    QjsRuntime *qrt = qjs_runtime_create(1024 * 1024);
    qjs_set_interrupt_limit(qrt, 100);
    JSContext *ctx = qjs_context_create(qrt);
    char *r = eval(ctx, "while(true){}");
    assert(r && (strstr(r, "interrupted") || strstr(r, "Interrupt")));
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
    expect(ctx,
        "var a = new BigInt64Array(new SharedArrayBuffer(16));"
        "a[0] = 0x123456789abcdefn;"
        "[typeof Atomics, a[0] === 0x123456789abcdefn].join()", "undefined,true", 1);
    JS_FreeContext(ctx);
    qjs_runtime_destroy(qrt);
}

int main(void) {
    TEST_INIT();
    RUN(test_eval_to_string);
    RUN(test_promise_resolve);
    RUN(test_console_log_circular);
    RUN(test_interrupt_limit);
    RUN(test_no_atomics);
    return 0;
}
