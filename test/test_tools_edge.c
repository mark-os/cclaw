#define _POSIX_C_SOURCE 200809L
#include "tools.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "test_util.h"

static char *dummy_handler(const char *arguments, void *user_data) {
    (void)arguments; (void)user_data;
    return strdup("ok");
}

static int free_fn_called = 0;
static void test_free_fn(void *data) {
    (void)data;
    free_fn_called = 1;
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


int main(void) {
    TEST_INIT();
    alarm(10);
    printf("test_tools_edge:\n");
    test_free_fn_called_on_cleanup();
    test_register_null_name();
    test_register_null_reg();
    test_lookup_null_args();
    test_schemas_null_reg();
    printf("All tools_edge tests passed.\n");
    return 0;
}
