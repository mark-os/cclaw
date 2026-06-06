#include "tools.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dummy_handler(const char *arguments, void *user_data) {
    (void)arguments;
    (void)user_data;
    char *r = malloc(8);
    if (r) strcpy(r, "ok");
    return r;
}

static void test_init(void) {
    ToolRegistry reg;
    tools_init(&reg);
    assert(reg.count == 0);
    printf("  PASS test_init\n");
}

static void test_register_and_lookup(void) {
    ToolRegistry reg;
    tools_init(&reg);

    int rc = tools_register(&reg, "shell_exec", "Run a command",
                            "{\"type\":\"object\"}", dummy_handler, NULL);
    assert(rc == 0);
    assert(reg.count == 1);

    ToolEntry *e = tools_lookup(&reg, "shell_exec");
    assert(e != NULL);
    assert(strcmp(e->name, "shell_exec") == 0);
    assert(strcmp(e->description, "Run a command") == 0);
    assert(e->handler == dummy_handler);

    tools_free(&reg);
    printf("  PASS test_register_and_lookup\n");
}

static void test_lookup_miss(void) {
    ToolRegistry reg;
    tools_init(&reg);

    tools_register(&reg, "file_read", NULL, NULL, dummy_handler, NULL);
    ToolEntry *e = tools_lookup(&reg, "nonexistent");
    assert(e == NULL);

    tools_free(&reg);
    printf("  PASS test_lookup_miss\n");
}

static void test_schemas(void) {
    ToolRegistry reg;
    tools_init(&reg);

    tools_register(&reg, "tool_a", "desc_a", "{\"type\":\"object\"}", dummy_handler, NULL);
    tools_register(&reg, "tool_b", "desc_b", NULL, dummy_handler, NULL);

    ToolSchema schemas[TOOLS_MAX];
    size_t count = tools_schemas(&reg, schemas, TOOLS_MAX);
    assert(count == 2);
    assert(strcmp(schemas[0].name, "tool_a") == 0);
    assert(strcmp(schemas[0].description, "desc_a") == 0);
    assert(strcmp(schemas[0].parameters_json, "{\"type\":\"object\"}") == 0);
    assert(strcmp(schemas[1].name, "tool_b") == 0);
    assert(schemas[1].parameters_json == NULL);

    tools_free(&reg);
    printf("  PASS test_schemas\n");
}

static void test_register_full(void) {
    ToolRegistry reg;
    tools_init(&reg);

    for (int i = 0; i < TOOLS_MAX; i++) {
        char name[16];
        snprintf(name, sizeof(name), "tool_%d", i);
        int rc = tools_register(&reg, name, NULL, NULL, dummy_handler, NULL);
        assert(rc == 0);
    }
    /* Registry full — next register should fail */
    int rc = tools_register(&reg, "overflow", NULL, NULL, dummy_handler, NULL);
    assert(rc == -1);
    assert(reg.count == TOOLS_MAX);

    tools_free(&reg);
    printf("  PASS test_register_full\n");
}

static void test_handler_dispatch(void) {
    ToolRegistry reg;
    tools_init(&reg);

    tools_register(&reg, "test_tool", NULL, NULL, dummy_handler, NULL);
    ToolEntry *e = tools_lookup(&reg, "test_tool");
    assert(e != NULL);

    char *result = e->handler("{}", e->user_data);
    assert(result != NULL);
    assert(strcmp(result, "ok") == 0);
    free(result);

    tools_free(&reg);
    printf("  PASS test_handler_dispatch\n");
}

int main(void) {
    printf("test_tools:\n");
    test_init();
    test_register_and_lookup();
    test_lookup_miss();
    test_schemas();
    test_register_full();
    test_handler_dispatch();
    printf("All tool registry tests passed.\n");
    return 0;
}
