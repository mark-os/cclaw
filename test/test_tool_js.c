/* js_eval through the in-process tier leaf (host mode, no network). */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "test_util.h"

/* want == NULL means the result must start with "error:". */
static void test_eval_cases(void) {
    static const struct { const char *args; const char *want; } cases[] = {
        { "{\"code\":\"1 + 2\"}",               "3" },
        { "{\"code\":\"'hello' + ' world'\"}",  "hello world" },
        { "{\"code\":\"var x = 1\"}",           "undefined" },
        { "{\"code\":\"function(\"}",           NULL },   /* syntax error */
        { "{\"code\":\"undefined_var.foo\"}",   NULL },   /* runtime error */
        { "{\"notcode\":\"x\"}",                NULL },   /* missing code */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *r = test_js_eval_run_json(cases[i].args);
        int ok = r && (cases[i].want ? strcmp(r, cases[i].want) == 0
                                     : strncmp(r, "error:", 6) == 0);
        if (!ok) printf("    row %zu %s got: %s\n", i, cases[i].args, r ? r : "NULL");
        assert(ok);
        free(r);
    }
}

/* js_eval must dispatch to the sandboxed JS tier, never run in the parent. */
static void test_register(void) {
    ToolRegistry reg;
    tools_init(&reg);
    assert(tool_js_eval_register(&reg, NULL) == 0);
    ToolEntry *e = tools_lookup(&reg, "js_eval");
    assert(e);
    assert(e->recipe.vehicle == EXEC_SANDBOX && e->recipe.tier == SBX_JS);
    tools_free(&reg);
}

/* A failed http_request returns {status:-1, body:"", error} — not a thrown
 * exception. body is always a string so grep-via-JS (.body.match) is safe. */
static void test_http_request_error_returns_object(void) {
    char *r = test_js_eval_run_json(
        "{\"code\":\"var x=http_request('http://127.0.0.1:1/');"
        "JSON.stringify([typeof x,x.body,typeof x.body,!!x.error])\"}");
    assert(r && strncmp(r, "error:", 6) != 0);
    assert(strstr(r, "[\"object\",\"\",\"string\",true]"));
    free(r);
}

int main(void) {
    TEST_INIT();
    printf("test_tool_js:\n");
    RUN(test_eval_cases);
    RUN(test_http_request_error_returns_object);
    RUN(test_register);
    return 0;
}
