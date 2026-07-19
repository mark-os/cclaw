#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "js_http_fetch.h"
#include "test_util.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  " name "... "); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

/* Egress (host/IP/redirect gating) is no longer a pre-flight in js_http_fetch —
 * it moved to the broker proxy's per-hop decide() (see test_proxy_decide.c).
 * The only validation left here is the scheme guard: http:// or https:// only. */
static void test_invalid_scheme(void) {
    TEST("invalid_scheme");
    JsHttpResult r = js_http_fetch_exec("ftp://example.com/file", "GET", NULL, NULL);
    if (r.status != -1) { FAIL("expected error"); js_http_result_free(&r); return; }
    if (!r.error || !strstr(r.error, "http://")) {
        FAIL(r.error ? r.error : "NULL"); js_http_result_free(&r); return;
    }
    js_http_result_free(&r);
    PASS();
}

static void test_null_url(void) {
    TEST("null_url");
    JsHttpResult r = js_http_fetch_exec(NULL, "GET", NULL, NULL);
    if (r.status != -1) { FAIL("expected error"); js_http_result_free(&r); return; }
    if (!r.error) { FAIL("expected error message"); js_http_result_free(&r); return; }
    js_http_result_free(&r);
    PASS();
}

/* Runtime create/destroy lifecycle (hosts feed the proxy, not the runtime) */
static void test_runtime_set_hosts(void) {
    TEST("runtime_set_hosts");
    JsSessionRuntime *rt = js_runtime_create();
    if (!rt) { FAIL("create failed"); return; }
    js_runtime_destroy(rt);
    PASS();
}

/* In-process js_eval compute: arithmetic returns the last expression value. No
 * fork, no re-exec, no network. */
static void test_js_eval_compute(void) {
    TEST("js_eval_compute");
    char *r = test_js_eval_run_json("{\"code\":\"6 * 7\"}");
    if (!r) { FAIL("NULL result"); return; }
    if (strcmp(r, "42") != 0) { FAIL(r); free(r); return; }
    free(r);
    PASS();
}

/* In-process js_eval: console.log output is captured. */
static void test_js_eval_console(void) {
    TEST("js_eval_console");
    char *r = test_js_eval_run_json("{\"code\":\"console.log('hi'); undefined\"}");
    if (!r) { FAIL("NULL result"); return; }
    if (!strstr(r, "hi")) { FAIL(r); free(r); return; }
    free(r);
    PASS();
}

/* js_eval requires code or filename. */
static void test_js_eval_requires_arg(void) {
    TEST("js_eval_requires_arg");
    char *r = test_js_eval_run_json("{}");
    if (!r) { FAIL("NULL result"); return; }
    if (!strstr(r, "error")) { FAIL(r); free(r); return; }
    free(r);
    PASS();
}

int main(void) {
    TEST_INIT();
    printf("test_js_http_fetch:\n");
    test_invalid_scheme();
    test_null_url();
    test_runtime_set_hosts();
    test_js_eval_compute();
    test_js_eval_console();
    test_js_eval_requires_arg();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
