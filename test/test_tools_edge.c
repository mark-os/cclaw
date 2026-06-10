#define _POSIX_C_SOURCE 200809L
#include "tools.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static char *dummy_handler(const char *arguments, void *user_data) {
    (void)arguments; (void)user_data;
    return strdup("ok");
}

static int free_fn_called = 0;
static void test_free_fn(void *data) {
    (void)data;
    free_fn_called = 1;
}

static void test_whitelist_null_allows_all(void) {
    assert(tools_is_whitelisted("anything", NULL, 0) == 1);
    printf("  PASS test_whitelist_null_allows_all\n");
}

static void test_whitelist_null_name(void) {
    const char *wl[] = {"shell_exec"};
    assert(tools_is_whitelisted(NULL, wl, 1) == 0);
    printf("  PASS test_whitelist_null_name\n");
}

static void test_whitelist_match(void) {
    const char *wl[] = {"shell_exec", "file_read", "file_write"};
    assert(tools_is_whitelisted("file_read", wl, 3) == 1);
    assert(tools_is_whitelisted("file_write", wl, 3) == 1);
    assert(tools_is_whitelisted("shell_exec", wl, 3) == 1);
    assert(tools_is_whitelisted("js_eval", wl, 3) == 0);
    printf("  PASS test_whitelist_match\n");
}

static void test_schemas_filtered(void) {
    ToolRegistry reg;
    tools_init(&reg);
    tools_register(&reg, "shell_exec", "run cmd", NULL, dummy_handler, NULL);
    tools_register(&reg, "file_read", "read file", NULL, dummy_handler, NULL);
    tools_register(&reg, "js_eval", "eval js", NULL, dummy_handler, NULL);

    const char *wl[] = {"shell_exec", "js_eval"};
    ToolSchema schemas[TOOLS_MAX];
    size_t count = tools_schemas_filtered(&reg, wl, 2, schemas, TOOLS_MAX);
    assert(count == 2);
    assert(strcmp(schemas[0].name, "shell_exec") == 0);
    assert(strcmp(schemas[1].name, "js_eval") == 0);

    tools_free(&reg);
    printf("  PASS test_schemas_filtered\n");
}

static void test_schemas_filtered_null_whitelist(void) {
    ToolRegistry reg;
    tools_init(&reg);
    tools_register(&reg, "a", NULL, NULL, dummy_handler, NULL);
    tools_register(&reg, "b", NULL, NULL, dummy_handler, NULL);

    ToolSchema schemas[TOOLS_MAX];
    size_t count = tools_schemas_filtered(&reg, NULL, 0, schemas, TOOLS_MAX);
    assert(count == 2);
    assert(schemas[0].name != NULL);

    tools_free(&reg);
    printf("  PASS test_schemas_filtered_null_whitelist\n");
}

static void test_free_fn_called_on_cleanup(void) {
    ToolRegistry reg;
    tools_init(&reg);
    tools_register(&reg, "test", NULL, NULL, dummy_handler, (void *)0x1);
    reg.entries[0].free_fn = test_free_fn;

    free_fn_called = 0;
    tools_free(&reg);
    assert(free_fn_called == 1);
    printf("  PASS test_free_fn_called_on_cleanup\n");
}

static void test_register_null_name(void) {
    ToolRegistry reg;
    tools_init(&reg);
    int rc = tools_register(&reg, NULL, "desc", NULL, dummy_handler, NULL);
    assert(rc == -1);
    assert(reg.count == 0);
    printf("  PASS test_register_null_name\n");
}

static void test_register_null_reg(void) {
    int rc = tools_register(NULL, "test", NULL, NULL, dummy_handler, NULL);
    assert(rc == -1);
    printf("  PASS test_register_null_reg\n");
}

static void test_lookup_null_args(void) {
    ToolRegistry reg;
    tools_init(&reg);
    assert(tools_lookup(NULL, "x") == NULL);
    assert(tools_lookup(&reg, NULL) == NULL);
    printf("  PASS test_lookup_null_args\n");
}

static void test_schemas_null_reg(void) {
    ToolSchema schemas[TOOLS_MAX];
    size_t count = tools_schemas(NULL, schemas, TOOLS_MAX);
    assert(count == 0);
    printf("  PASS test_schemas_null_reg\n");
}

static void test_whitelist_no_match(void) {
    ToolRegistry reg;
    tools_init(&reg);
    tools_register(&reg, "shell_exec", "run cmd", "{}", dummy_handler, NULL);

    const char *wl[] = {"nonexistent"};
    ToolSchema schemas[TOOLS_MAX];
    size_t count = tools_schemas_filtered(&reg, wl, 1, schemas, TOOLS_MAX);
    assert(count == 0);
    assert(tools_is_whitelisted("shell_exec", wl, 1) == 0);
    assert(tools_is_whitelisted(NULL, wl, 1) == 0);

    tools_free(&reg);
    printf("  PASS test_whitelist_no_match\n");
}

int main(void) {
    alarm(10);
    printf("test_tools_edge:\n");
    test_whitelist_null_allows_all();
    test_whitelist_null_name();
    test_whitelist_match();
    test_whitelist_no_match();
    test_schemas_filtered();
    test_schemas_filtered_null_whitelist();
    test_free_fn_called_on_cleanup();
    test_register_null_name();
    test_register_null_reg();
    test_lookup_null_args();
    test_schemas_null_reg();
    printf("All tools_edge tests passed.\n");
    return 0;
}
