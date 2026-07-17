#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "test_util.h"

static int tests_run = 0;
static int tests_passed = 0;

/* js_eval now evaluates in-process via qjs_eval_run — no fork, no --qjs_eval
 * re-exec. These unit tests call the handler directly (host mode, no network). */

#define TEST(name) do { tests_run++; printf("  " name "... "); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

static void test_basic_eval(void) {
    TEST("basic_eval");
    char *r = test_js_eval_run_json("{\"code\":\"1 + 2\"}");
    if (!r || strcmp(r, "3") != 0) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    PASS();
}

static void test_string_result(void) {
    TEST("string_result");
    char *r = test_js_eval_run_json("{\"code\":\"'hello' + ' world'\"}");
    if (!r || strcmp(r, "hello world") != 0) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    PASS();
}

static void test_json_stringify(void) {
    TEST("json_stringify");
    char *r = test_js_eval_run_json("{\"code\":\"JSON.stringify({a:1})\"}");
    if (!r || strcmp(r, "{\"a\":1}") != 0) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    PASS();
}

static void test_syntax_error(void) {
    TEST("syntax_error");
    char *r = test_js_eval_run_json("{\"code\":\"function(\"}");
    if (!r || strncmp(r, "error:", 6) != 0) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    PASS();
}

static void test_runtime_error(void) {
    TEST("runtime_error");
    char *r = test_js_eval_run_json("{\"code\":\"undefined_var.foo\"}");
    if (!r || strncmp(r, "error:", 6) != 0) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    PASS();
}

static void test_missing_code(void) {
    TEST("missing_code");
    char *r = test_js_eval_run_json("{\"notcode\":\"x\"}");
    if (!r || strncmp(r, "error:", 6) != 0) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    PASS();
}

static void test_invalid_json(void) {
    TEST("invalid_json");
    char *r = test_js_eval_run_json("not json");
    if (!r || strcmp(r, "error: invalid tool arguments") != 0) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    PASS();
}

static void test_undefined_result(void) {
    TEST("undefined_result");
    char *r = test_js_eval_run_json("{\"code\":\"var x = 1\"}");
    if (!r || strcmp(r, "undefined") != 0) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    PASS();
}

static void test_register(void) {
    TEST("register");
    ToolRegistry reg;
    tools_init(&reg);
    int rc = tool_js_eval_register(&reg, NULL);
    if (rc != 0) { FAIL("register failed"); tools_free(&reg); return; }
    ToolEntry *e = tools_lookup(&reg, "js_eval");
    if (!e) { FAIL("lookup failed"); tools_free(&reg); return; }
    if (e->recipe.vehicle != EXEC_SANDBOX || e->recipe.tier != SBX_JS) {
        FAIL("expected EXEC_SANDBOX/SBX_JS recipe"); tools_free(&reg); return;
    }
    tools_free(&reg);
    PASS();
}

/* A failed http_request returns {status:-1, body:"", error} — not a thrown
 * exception. body is always a string so grep-via-JS (.body.match) is safe. */
static void test_http_request_error_returns_object(void) {
    TEST("http_request_error_object");
    char *r = test_js_eval_run_json(
        "{\"code\":\"var x=http_request('http://127.0.0.1:1/');"
        "JSON.stringify([typeof x,x.body,typeof x.body,!!x.error])\"}");
    /* not a thrown error, and shape is {object, body:'', string, hasError} */
    if (!r || strncmp(r, "error:", 6) == 0) { FAIL(r ? r : "NULL"); free(r); return; }
    if (!strstr(r, "[\"object\",\"\",\"string\",true]")) { FAIL(r); free(r); return; }
    free(r);
    PASS();
}

int main(void) {
    TEST_INIT();
    printf("test_tool_js:\n");
    test_basic_eval();
    test_http_request_error_returns_object();
    test_string_result();
    test_json_stringify();
    test_syntax_error();
    test_runtime_error();
    test_missing_code();
    test_invalid_json();
    test_undefined_result();
    test_register();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
