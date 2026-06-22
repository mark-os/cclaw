/* Test qjs_helpers — the shared QuickJS helper layer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qjs_helpers.h"

static int failures = 0;
#define ASSERT(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while(0)

static void test_runtime_create_destroy(void) {
    QjsRuntime *qrt = qjs_runtime_create(1024 * 1024);
    ASSERT(qrt != NULL, "runtime create");
    ASSERT(qrt->rt != NULL, "runtime has JSRuntime");
    qjs_runtime_destroy(qrt);
}

static void test_context_profiles(void) {
    QjsRuntime *qrt = qjs_runtime_create(2 * 1024 * 1024);

    /* Hooks profile — minimal intrinsics */
    JSContext *ctx = qjs_context_create(qrt, QJS_PROFILE_HOOKS);
    ASSERT(ctx != NULL, "hooks context create");
    /* JSON should work */
    char *r = qjs_eval_to_string(ctx, "JSON.stringify({a:1})", NULL);
    ASSERT(r && strcmp(r, "{\"a\":1}") == 0, "hooks profile has JSON");
    free(r);
    JS_FreeContext(ctx);

    /* Eval profile — full intrinsics */
    ctx = qjs_context_create(qrt, QJS_PROFILE_EVAL);
    ASSERT(ctx != NULL, "eval context create");
    r = qjs_eval_to_string(ctx, "new Date(0).toISOString()", NULL);
    ASSERT(r && strcmp(r, "1970-01-01T00:00:00.000Z") == 0, "eval profile has Date");
    free(r);
    JS_FreeContext(ctx);

    /* Channel profile — full + Promise */
    ctx = qjs_context_create(qrt, QJS_PROFILE_CHANNEL);
    ASSERT(ctx != NULL, "channel context create");
    r = qjs_eval_to_string(ctx, "typeof Promise", NULL);
    ASSERT(r && strcmp(r, "function") == 0, "channel profile has Promise");
    free(r);
    JS_FreeContext(ctx);

    qjs_runtime_destroy(qrt);
}

static void test_eval_to_string(void) {
    QjsRuntime *qrt = qjs_runtime_create(1024 * 1024);
    JSContext *ctx = qjs_context_create(qrt, QJS_PROFILE_EVAL);

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

static void test_global_json(void) {
    QjsRuntime *qrt = qjs_runtime_create(1024 * 1024);
    JSContext *ctx = qjs_context_create(qrt, QJS_PROFILE_EVAL);

    /* Set JSON as global */
    int rc = qjs_set_global_json(ctx, "data", "{\"x\":42,\"arr\":[1,2,3]}");
    ASSERT(rc == 0, "set_global_json returns 0");

    /* Read it back via eval */
    char *r = qjs_eval_to_string(ctx, "String(data.x)", NULL);
    ASSERT(r && strcmp(r, "42") == 0, "global json property access");
    free(r);

    /* Get it back as JSON */
    char *json = qjs_get_global_json(ctx, "data");
    ASSERT(json != NULL, "get_global_json not null");
    ASSERT(strstr(json, "\"x\":42"), "get_global_json contains x:42");
    free(json);

    /* Non-existent returns NULL */
    json = qjs_get_global_json(ctx, "nonexistent");
    ASSERT(json == NULL, "get_global_json nonexistent is NULL");

    JS_FreeContext(ctx);
    qjs_runtime_destroy(qrt);
}

static void test_interrupt_limit(void) {
    QjsRuntime *qrt = qjs_runtime_create(1024 * 1024);
    qjs_set_interrupt_limit(qrt, 100);
    JSContext *ctx = qjs_context_create(qrt, QJS_PROFILE_EVAL);

    /* Infinite loop should be interrupted */
    char *r = qjs_eval_to_string(ctx, "while(true){}", NULL);
    ASSERT(r != NULL, "interrupt returns error string");
    ASSERT(strstr(r, "interrupted") || strstr(r, "Interrupt"), "interrupt message");
    free(r);

    JS_FreeContext(ctx);
    qjs_runtime_destroy(qrt);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_runtime_create_destroy();
    test_context_profiles();
    test_eval_to_string();
    test_global_json();
    test_interrupt_limit();

    if (failures == 0)
        printf("PASS: test_quickjs (%d checks)\n", 0);
    else
        printf("FAIL: test_quickjs (%d failures)\n", failures);
    return failures ? 1 : 0;
}
