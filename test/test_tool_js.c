#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "tool_js.h"

static int tests_run = 0;
static int tests_passed = 0;

/* js_eval now forks `cclaw --qjs_eval`. Point the handler at the real cclaw
 * binary (sibling of this test binary) and run it in host mode (no sandbox),
 * so these unit tests exercise the real subprocess path without needing a
 * working userns sandbox or any network. */
static void setup_qjs_env(void) {
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) { fprintf(stderr, "readlink /proc/self/exe failed\n"); exit(2); }
    self[n] = '\0';
    char *slash = strrchr(self, '/');
    if (slash) slash[1] = '\0'; else self[0] = '\0';
    char cclaw_path[4128];
    snprintf(cclaw_path, sizeof(cclaw_path), "%scclaw", self);
    setenv("CCLAW_QJS_EXE", cclaw_path, 1);
    setenv("CCLAW_QJS_HOST", "1", 1);
}

#define TEST(name) do { tests_run++; printf("  " name "... "); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

static void test_basic_eval(void) {
    TEST("basic_eval");
    char *r = tool_js_eval_handler("{\"code\":\"1 + 2\"}", NULL);
    if (!r || strcmp(r, "3") != 0) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    PASS();
}

static void test_string_result(void) {
    TEST("string_result");
    char *r = tool_js_eval_handler("{\"code\":\"'hello' + ' world'\"}", NULL);
    if (!r || strcmp(r, "hello world") != 0) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    PASS();
}

static void test_json_stringify(void) {
    TEST("json_stringify");
    char *r = tool_js_eval_handler("{\"code\":\"JSON.stringify({a:1})\"}", NULL);
    if (!r || strcmp(r, "{\"a\":1}") != 0) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    PASS();
}

static void test_syntax_error(void) {
    TEST("syntax_error");
    char *r = tool_js_eval_handler("{\"code\":\"function(\"}", NULL);
    if (!r || strncmp(r, "error:", 6) != 0) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    PASS();
}

static void test_runtime_error(void) {
    TEST("runtime_error");
    char *r = tool_js_eval_handler("{\"code\":\"undefined_var.foo\"}", NULL);
    if (!r || strncmp(r, "error:", 6) != 0) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    PASS();
}

static void test_missing_code(void) {
    TEST("missing_code");
    char *r = tool_js_eval_handler("{\"notcode\":\"x\"}", NULL);
    if (!r || strncmp(r, "error:", 6) != 0) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    PASS();
}

static void test_invalid_json(void) {
    TEST("invalid_json");
    char *r = tool_js_eval_handler("not json", NULL);
    if (!r || strncmp(r, "error:", 6) != 0) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    PASS();
}

static void test_undefined_result(void) {
    TEST("undefined_result");
    char *r = tool_js_eval_handler("{\"code\":\"var x = 1\"}", NULL);
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
    tools_free(&reg);
    PASS();
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setup_qjs_env();
    printf("test_tool_js:\n");
    test_basic_eval();
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
