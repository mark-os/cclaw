#define _POSIX_C_SOURCE 200809L
#include "tools.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* T78: per-agent tool whitelist filtering (V20, §I.tool) */

static char *dummy_handler(const char *args, void *ud) {
    (void)args; (void)ud;
    return strdup("ok");
}

static void test_null_whitelist_allows_all(void) {
    ToolRegistry reg;
    tools_init(&reg);
    tools_register(&reg, "shell_exec", "run cmd", "{}", dummy_handler, NULL);
    tools_register(&reg, "file_read", "read file", "{}", dummy_handler, NULL);

    size_t count = 0;
    const ToolSchema *s = tools_schemas_filtered(&reg, NULL, 0, &count);
    assert(count == 2);
    assert(s != NULL);

    assert(tools_is_whitelisted("shell_exec", NULL, 0) == 1);
    assert(tools_is_whitelisted("file_read", NULL, 0) == 1);

    tools_free(&reg);
}

static void test_whitelist_filters(void) {
    ToolRegistry reg;
    tools_init(&reg);
    tools_register(&reg, "shell_exec", "run cmd", "{}", dummy_handler, NULL);
    tools_register(&reg, "file_read", "read file", "{}", dummy_handler, NULL);
    tools_register(&reg, "file_write", "write file", "{}", dummy_handler, NULL);

    const char *whitelist[] = {"file_read", "file_write"};
    size_t count = 0;
    const ToolSchema *s = tools_schemas_filtered(&reg, whitelist, 2, &count);
    assert(count == 2);
    assert(strcmp(s[0].name, "file_read") == 0);
    assert(strcmp(s[1].name, "file_write") == 0);

    assert(tools_is_whitelisted("file_read", whitelist, 2) == 1);
    assert(tools_is_whitelisted("shell_exec", whitelist, 2) == 0);

    tools_free(&reg);
}

static void test_empty_whitelist_allows_all(void) {
    ToolRegistry reg;
    tools_init(&reg);
    tools_register(&reg, "js_eval", "eval js", "{}", dummy_handler, NULL);

    const char **empty = NULL;
    size_t count = 0;
    const ToolSchema *s = tools_schemas_filtered(&reg, empty, 0, &count);
    assert(count == 1);
    assert(s != NULL);

    tools_free(&reg);
}

static void test_whitelist_no_match(void) {
    ToolRegistry reg;
    tools_init(&reg);
    tools_register(&reg, "shell_exec", "run cmd", "{}", dummy_handler, NULL);

    const char *whitelist[] = {"nonexistent"};
    size_t count = 99;
    tools_schemas_filtered(&reg, whitelist, 1, &count);
    assert(count == 0);

    assert(tools_is_whitelisted("shell_exec", whitelist, 1) == 0);
    assert(tools_is_whitelisted(NULL, whitelist, 1) == 0);

    tools_free(&reg);
}

int main(void) {
    test_null_whitelist_allows_all();
    test_whitelist_filters();
    test_empty_whitelist_allows_all();
    test_whitelist_no_match();
    printf("test_tool_whitelist: all passed\n");
    return 0;
}
