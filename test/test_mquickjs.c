#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mquickjs.h"

extern const JSSTDLibraryDef js_std_library;

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  " name "... "); \
} while(0)

#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

/* V5: 1MB heap cap */
#define HEAP_SIZE (1024 * 1024)

static void test_context_create(void) {
    TEST("context_create");
    void *heap = malloc(HEAP_SIZE);
    JSContext *ctx = JS_NewContext(heap, HEAP_SIZE, &js_std_library);
    if (!ctx) { FAIL("JS_NewContext returned NULL"); free(heap); return; }
    JS_FreeContext(ctx);
    free(heap);
    PASS();
}

static void test_eval_int(void) {
    TEST("eval_int");
    void *heap = malloc(HEAP_SIZE);
    JSContext *ctx = JS_NewContext(heap, HEAP_SIZE, &js_std_library);
    JSValue val = JS_Eval(ctx, "1 + 2", 5, "<test>", JS_EVAL_RETVAL);
    if (JS_IsException(val)) { FAIL("exception"); JS_FreeContext(ctx); free(heap); return; }
    int result;
    if (JS_ToInt32(ctx, &result, val) != 0) { FAIL("ToInt32 failed"); JS_FreeContext(ctx); free(heap); return; }
    if (result != 3) { FAIL("expected 3"); JS_FreeContext(ctx); free(heap); return; }
    JS_FreeContext(ctx);
    free(heap);
    PASS();
}

static void test_eval_string(void) {
    TEST("eval_string");
    void *heap = malloc(HEAP_SIZE);
    JSContext *ctx = JS_NewContext(heap, HEAP_SIZE, &js_std_library);
    JSValue val = JS_Eval(ctx, "'hello' + ' world'", 18, "<test>", JS_EVAL_RETVAL);
    if (JS_IsException(val)) { FAIL("exception"); JS_FreeContext(ctx); free(heap); return; }
    JSCStringBuf buf;
    const char *str = JS_ToCString(ctx, val, &buf);
    if (!str) { FAIL("ToCString failed"); JS_FreeContext(ctx); free(heap); return; }
    if (strcmp(str, "hello world") != 0) { FAIL("wrong string"); JS_FreeContext(ctx); free(heap); return; }
    JS_FreeContext(ctx);
    free(heap);
    PASS();
}

static void test_eval_math(void) {
    TEST("eval_math");
    void *heap = malloc(HEAP_SIZE);
    JSContext *ctx = JS_NewContext(heap, HEAP_SIZE, &js_std_library);
    JSValue val = JS_Eval(ctx, "Math.floor(3.7)", 15, "<test>", JS_EVAL_RETVAL);
    if (JS_IsException(val)) { FAIL("exception"); JS_FreeContext(ctx); free(heap); return; }
    int result;
    if (JS_ToInt32(ctx, &result, val) != 0) { FAIL("ToInt32 failed"); JS_FreeContext(ctx); free(heap); return; }
    if (result != 3) { FAIL("expected 3"); JS_FreeContext(ctx); free(heap); return; }
    JS_FreeContext(ctx);
    free(heap);
    PASS();
}

static void test_eval_json(void) {
    TEST("eval_json");
    void *heap = malloc(HEAP_SIZE);
    JSContext *ctx = JS_NewContext(heap, HEAP_SIZE, &js_std_library);
    const char *code = "JSON.stringify({a: 1, b: 'x'})";
    JSValue val = JS_Eval(ctx, code, strlen(code), "<test>", JS_EVAL_RETVAL);
    if (JS_IsException(val)) { FAIL("exception"); JS_FreeContext(ctx); free(heap); return; }
    JSCStringBuf buf;
    const char *str = JS_ToCString(ctx, val, &buf);
    if (!str) { FAIL("ToCString failed"); JS_FreeContext(ctx); free(heap); return; }
    if (strcmp(str, "{\"a\":1,\"b\":\"x\"}") != 0) { FAIL(str); JS_FreeContext(ctx); free(heap); return; }
    JS_FreeContext(ctx);
    free(heap);
    PASS();
}

int main(void) {
    printf("test_mquickjs:\n");
    test_context_create();
    test_eval_int();
    test_eval_string();
    test_eval_math();
    test_eval_json();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
